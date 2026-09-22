// Net 的 TLS/HTTP2 自检示例：HttpsServer 起服与证书拒因、ALPN 协商 h2 的端到端往返、明文 h2c 先验知识、
// 一条连接上的多路复用、HTTP/2 通告的服务端上限、优雅关停的 GOAWAY，以及 h2 上的 request-id 与指标端点
#include "Base/Exception/Exception.h"
#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/HttpMetricsEndpoint.h"
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpRequestId.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/HttpServerStats.h"
#include "Net/Http/HttpsServer.h"
#include "Net/Http/Router.h"
#include "Net/Http2/Hpack.h"
#include "Net/Http2/Http2Connection.h"
#include "Net/Http2/Http2Frame.h"
#include "Net/Tcp/TcpServer.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/System/PlatformError.h"
#include "common/SampleSupport.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace AsynGyanis;

namespace
{
    /// 本示例的端口偏移；同一台机器上多个示例并行时由进程号错开。
    /// --port 给的是**基准端口**：三台服务器依次占用 basePort、basePort+1、basePort+2
    constexpr std::uint16_t kSamplePortOffset = 60;

    /// 单次等待的上限：协议往返都在本机回环上，超过这个时长就是没通
    constexpr std::chrono::milliseconds kWaitTimeout{5000};

    /// 两次轮询之间的间隔（只在「读一眼有没有新字节」这类有界等待里用）
    constexpr std::chrono::milliseconds kPollInterval{2};

    /// 自动生成的 request-id 长度：4 位十六进制前缀 + '-' + 16 位十六进制序号
    constexpr std::size_t kGeneratedRequestIdLength = 21;

    /// request-id 里分隔符的下标
    constexpr std::size_t kRequestIdSeparatorIndex = 4;

    /// 多路复用一步并发出去的流数
    constexpr std::size_t kMultiplexedStreamCount = 4;

    /// 优雅关停时给在途请求留出的时长
    constexpr std::chrono::milliseconds kGracefulDrainTimeout{2000};

    /// 在途请求的处理器故意占住的时长：短于 kGracefulDrainTimeout，长到足以让 drain 真的等到它
    constexpr std::chrono::milliseconds kInflightWorkDuration{300};

    /// 客户端为本连接放行的接收窗口增量：/metrics 的文本可能超过 65535 的规范初值
    constexpr std::uint32_t kClientConnectionCreditByteCount = 1024U * 1024U;

    /// 客户端通告的流级初始窗口：同上，别让响应卡在流控上而不是卡在协议上
    constexpr std::uint32_t kClientInitialWindowSizeByteCount = 1024U * 1024U;

    /**
     * @brief 跑一步可能抛异常的动作：抛了就折成「这一步失败」并把原因留在日志里
     * @tparam Body 判定体：返回 true 表示这一步通过
     * @param step 用在失败日志里的说明
     * @param body 判定体
     * @return bool 这一步是否通过（异常一律算没过）
     */
    template <typename Body>
    bool runGuarded(const std::string_view step, const Body &body)
    {
        try
        {
            return body();
        } catch (const Base::Exception &failure)
        {
            LOG_ERROR_FMT("{}：抛出框架异常。原因：{}", step, failure.what());
            return false;
        } catch (const std::exception &failure)
        {
            LOG_ERROR_FMT("{}：抛出非框架异常。原因：{}", step, failure.what());
            return false;
        }
    }

    /**
     * @brief 证书夹具的路径集合
     */
    struct TlsMaterial
    {
        std::filesystem::path certificateFile; ///< 服务端出示的证书（SAN 含 IP:127.0.0.1）
        std::filesystem::path keyFile;         ///< 与之配套的私钥
        std::filesystem::path foreignCertificateFile; ///< 另一张自签证书：拿它当受信根必然不认服务端那张

        /// @return bool 三份材料都在（缺一份，后面的 TLS 步骤就无从判起）
        [[nodiscard]] bool isComplete() const
        {
            std::error_code error;
            return std::filesystem::exists(certificateFile, error) && std::filesystem::exists(keyFile, error)
                   && std::filesystem::exists(foreignCertificateFile, error);
        }

        /// @return std::string 便于写进日志的一行摘要
        [[nodiscard]] std::string describe() const
        {
            return "证书=" + certificateFile.string() + " 私钥=" + keyFile.string()
                   + " 不受信根=" + foreignCertificateFile.string();
        }
    };

    /**
     * @brief 定位仓库里的证书夹具目录：先从可执行文件所在位置往上找，再退回当前工作目录
     * @param executablePath argv[0]
     * @return std::filesystem::path 夹具目录
     */
    std::filesystem::path locateFixtureDirectory(const char *executablePath)
    {
        std::error_code error;
        std::filesystem::path candidate = std::filesystem::weakly_canonical(std::filesystem::absolute(executablePath), error);
        for (std::size_t depth = 0; depth < 8 && !error; ++depth)
        {
            const std::filesystem::path fixtures = candidate / "tests" / "Core" / "fixtures";
            if (std::filesystem::exists(fixtures / "test_ip_cert.pem", error))
            {
                return fixtures;
            }
            if (candidate == candidate.parent_path())
            {
                break;
            }
            candidate = candidate.parent_path();
        }
        // runner 从仓库根启动时这条路径直接命中；上面那条则覆盖「从构建目录里直接跑」的情形
        return std::filesystem::path{"tests"} / "Core" / "fixtures";
    }

    /**
     * @brief 读证书路径：--cert/--key 可覆盖，默认取仓库自签夹具
     * @param argc 实参个数
     * @param argv 实参表
     * @return TlsMaterial 三份材料的路径
     */
    TlsMaterial readTlsMaterial(const int argc, char **argv)
    {
        const std::filesystem::path fixtures = locateFixtureDirectory(argv[0]);
        TlsMaterial                   material;
        material.certificateFile         = fixtures / "test_ip_cert.pem";
        material.keyFile                 = fixtures / "test_ip_key.pem";
        material.foreignCertificateFile  = fixtures / "test_localhost_cert.pem";
        for (int index = 1; index + 1 < argc; ++index)
        {
            const std::string_view option(argv[index]);
            if (option == "--cert")
            {
                material.certificateFile = argv[index + 1];
            } else if (option == "--key")
            {
                material.keyFile = argv[index + 1];
            }
        }
        return material;
    }

    // ============================================================================
    // 手拼 HTTP/2 客户端要用的帧与头块
    //
    // 框架的出站客户端只会说 HTTP/1.1（Net::HttpClient 不登记 ALPN，也没有 h2 通道），
    // 因此 TLS 上的 h2 与明文 h2c 两条路都只能自己发帧：帧与 HPACK 一律用生产编解码器，
    // 「服务端吐出的字节」由第二份实现解回来。
    // ============================================================================

    /// @return std::string 索引字段表示（RFC 7541 §6.1）
    std::string hpackIndexedField(const std::size_t index)
    {
        return Net::encodeHpackInteger(index, 7, 0x80);
    }

    /**
     * @brief 带静态表名字的字面量字段（RFC 7541 §6.2.1，名字索引由调用方给）
     * @param staticNameIndex 静态表下标（1..61）
     * @param value 头值
     * @return std::string 编码结果
     */
    std::string hpackLiteralField(const std::size_t staticNameIndex, const std::string_view value)
    {
        std::string bytes = Net::encodeHpackInteger(staticNameIndex, 6, 0x40);
        Net::appendHpackString(bytes, value);
        return bytes;
    }

    /**
     * @brief 名字与值都是字面量的字段（普通头部用）
     * @param name 头名（必须全小写，RFC 7540 §8.1.2）
     * @param value 头值
     * @return std::string 编码结果
     */
    std::string hpackLiteralField(const std::string_view name, const std::string_view value)
    {
        std::string bytes = Net::encodeHpackInteger(0, 6, 0x40);
        Net::appendHpackString(bytes, name);
        Net::appendHpackString(bytes, value);
        return bytes;
    }

    /**
     * @brief 拼一个 GET 请求的头块
     * @param path 请求路径
     * @param isOverTls true 时 :scheme 取 https，明文 h2c 取 http
     * @param extraHeaderFields 追加的普通头，按给定顺序排在伪头之后
     * @return std::string 头块字节
     */
    std::string makeGetRequestHeaderBlock(const std::string_view path, const bool isOverTls,
                                          const std::vector<Net::HpackHeaderField> &extraHeaderFields = {})
    {
        std::string headerBlock;
        headerBlock += hpackIndexedField(2);                                            // :method GET
        headerBlock += hpackIndexedField(isOverTls ? 7 : 6);                            // :scheme https / http
        headerBlock += path == "/" ? hpackIndexedField(4) : hpackLiteralField(4, path); // :path（"/" 有静态表项）
        headerBlock += hpackLiteralField(1, "localhost");                               // :authority（静态表只给名字）
        for (const Net::HpackHeaderField &field: extraHeaderFields)
        {
            headerBlock += hpackLiteralField(field.name, field.value);
        }
        return headerBlock;
    }

    /**
     * @brief 拼一个 POST 请求的头块（不带 END_STREAM：正文随后由 DATA 帧发出）
     * @param path 请求路径
     * @param isOverTls 同 makeGetRequestHeaderBlock()
     * @return std::string 头块字节
     */
    std::string makePostRequestHeaderBlock(const std::string_view path, const bool isOverTls)
    {
        std::string headerBlock;
        headerBlock += hpackLiteralField(2, "POST");
        headerBlock += hpackIndexedField(isOverTls ? 7 : 6);
        headerBlock += hpackLiteralField(4, path);
        headerBlock += hpackLiteralField(1, "localhost");
        return headerBlock;
    }

    /**
     * @brief 编一个 HEADERS 帧（头块在一帧内结束，不带优先级字段）
     * @param streamId 流号
     * @param headerBlock 头块字节
     * @param isEndStream 是否同时带 END_STREAM（表示这条请求没有正文）
     * @return std::string 完整帧字节
     */
    std::string makeRequestHeadersFrame(const std::uint32_t streamId, const std::string_view headerBlock, const bool isEndStream)
    {
        return Net::encodeHttp2HeadersFrame(
                Net::Http2HeadersPayload{.endStream = isEndStream, .endHeaders = true, .headerBlockFragment = std::string(headerBlock)},
                streamId);
    }

    /// @return std::string 客户端的初始 SETTINGS：把流级初始窗口放大到 1 MiB
    std::string makeClientSettingsFrame()
    {
        Net::Http2SettingsPayload payload;
        payload.parameters = {{static_cast<std::uint16_t>(Net::Http2SettingIdentifier::InitialWindowSize), kClientInitialWindowSizeByteCount}};
        return Net::encodeHttp2SettingsFrame(payload);
    }

    /// @return std::string 空的 SETTINGS ACK 帧（RFC 7540 §6.5 要求 ACK 负载为空）
    std::string makeSettingsAckFrame()
    {
        return Net::encodeHttp2SettingsFrame(Net::Http2SettingsPayload{.isAcknowledgement = true});
    }

    /// @return std::string 连接级 WINDOW_UPDATE：给服务端放行更多待发送的字节
    std::string makeConnectionWindowUpdateFrame()
    {
        return Net::encodeHttp2WindowUpdateFrame(Net::Http2WindowUpdatePayload{.windowSizeIncrement = kClientConnectionCreditByteCount}, 0U);
    }

    /// 一次读取的结论：与「对端关了 / 还没字节 / 通道坏了」三件事一一对应
    enum class ReadOutcome
    {
        Data,
        Idle,
        PeerClosed,
        Broken
    };

    /**
     * @brief 一条回环上的 HTTP/2 客户端连接（明文或 TLS），把收到的字节就地解成帧
     * @details 只在本线程推进：非阻塞描述符 + 有界轮询，任何一步都不会把主线程挂死。
     */
    class Http2LoopbackClient
    {
    public:
        Http2LoopbackClient(const Http2LoopbackClient &) = delete;
        Http2LoopbackClient &operator=(const Http2LoopbackClient &) = delete;

        ~Http2LoopbackClient()
        {
            closeNow();
        }

        /**
         * @brief 连一条明文连接（h2c 先验知识用）
         * @param port 服务端端口
         * @return std::unique_ptr<Http2LoopbackClient> 连上即返回；连不上时 isSocketConnected() 为 false
         */
        [[nodiscard]] static std::unique_ptr<Http2LoopbackClient> openCleartext(const std::uint16_t port)
        {
            std::unique_ptr<Http2LoopbackClient> client = std::unique_ptr<Http2LoopbackClient>(new Http2LoopbackClient());
            client->connectTo(port);
            return client;
        }

        /**
         * @brief 连一条 TLS 连接
         * @param port 服务端端口
         * @param offeredAlpnProtocol 登记进 ALPN 的协议名；空串表示完全不提供 ALPN
         * @param trustedCertificateFile 非空则校验服务端证书（并把它当唯一受信根），为空则不校验
         * @return std::unique_ptr<Http2LoopbackClient> 对象总能拿到，握手结论用 isHandshakeComplete() 问
         */
        [[nodiscard]] static std::unique_ptr<Http2LoopbackClient> openOverTls(const std::uint16_t port,
                                                                              const std::string_view offeredAlpnProtocol,
                                                                              const std::string &trustedCertificateFile)
        {
            std::unique_ptr<Http2LoopbackClient> client = std::unique_ptr<Http2LoopbackClient>(new Http2LoopbackClient());
            client->connectTo(port);
            if (client->isSocketConnected())
            {
                client->installTls(offeredAlpnProtocol, trustedCertificateFile);
                static_cast<void>(client->performHandshake());
            }
            return client;
        }

        [[nodiscard]] bool isSocketConnected() const noexcept
        {
            return Platform::FileDescriptor::isValid(m_descriptor);
        }

        /// 明文连接恒为 true；TLS 连接表示握手是否走完
        [[nodiscard]] bool isChannelReady() const noexcept
        {
            return isSocketConnected() && (m_ssl == nullptr || m_isHandshakeDone);
        }

        [[nodiscard]] bool isHandshakeComplete() const noexcept
        {
            return m_ssl != nullptr && m_isHandshakeDone;
        }

        /// 本端是否成功把协议名登记进 ALPN 扩展（没登记成功时「协商出 h2」这条判定就无从谈起）
        [[nodiscard]] bool isAlpnListAccepted() const noexcept
        {
            return m_isAlpnListAccepted;
        }

        [[nodiscard]] const std::string &handshakeFailureReason() const noexcept
        {
            return m_handshakeFailureReason;
        }

        /// 客户端视角的 ALPN 协商结果（握手未完成时为空串）
        [[nodiscard]] std::string selectedApplicationProtocol() const
        {
            if (m_ssl == nullptr)
            {
                return {};
            }
            const unsigned char *protocolName = nullptr;
            unsigned int         protocolNameLength = 0;
            SSL_get0_alpn_selected(m_ssl, &protocolName, &protocolNameLength);
            if (protocolName == nullptr || protocolNameLength == 0)
            {
                return {};
            }
            return std::string(reinterpret_cast<const char *>(protocolName), protocolNameLength);
        }

        /// 证书校验结果的英文原文（未启用校验或未失败时为空串）
        [[nodiscard]] std::string certificateVerifyFailureText() const
        {
            if (m_ssl == nullptr || !m_verifiesPeerCertificate)
            {
                return {};
            }
            const long verifyResult = SSL_get_verify_result(m_ssl);
            if (verifyResult == X509_V_OK)
            {
                return {};
            }
            const char *const rawText = X509_verify_cert_error_string(verifyResult);
            return rawText != nullptr ? std::string(rawText) : std::string{"证书校验未通过（无原因文本）"};
        }

        /**
         * @brief 把整段字节写出去（有界）
         * @param bytes 待发字节
         * @return true 全部字节已交给内核或 TLS 通道
         */
        bool sendBytes(const std::string_view bytes)
        {
            if (!isChannelReady())
            {
                return false;
            }
            std::size_t writtenLength = 0;
            return Samples::waitUntil(
                    [this, bytes, &writtenLength]()
                    {
                        while (writtenLength < bytes.size())
                        {
                            const std::size_t remainingLength = bytes.size() - writtenLength;
                            if (m_ssl != nullptr)
                            {
                                const int writeLength =
                                        SSL_write(m_ssl, bytes.data() + writtenLength, static_cast<int>(remainingLength));
                                if (writeLength > 0)
                                {
                                    writtenLength += static_cast<std::size_t>(writeLength);
                                    continue;
                                }
                                const int sslError = SSL_get_error(m_ssl, writeLength);
                                if (sslError != SSL_ERROR_WANT_READ && sslError != SSL_ERROR_WANT_WRITE)
                                {
                                    m_handshakeFailureReason = describeChannelFailure("写出字节");
                                    return true; // 硬失败：结束等待，由调用方看 writtenLength 判定
                                }
                                return false; // 还写得动，下一轮再来
                            }

                            const ssize_t sendLength =
                                    Platform::FileDescriptor::write(m_descriptor, bytes.data() + writtenLength, remainingLength);
                            if (sendLength > 0)
                            {
                                writtenLength += static_cast<std::size_t>(sendLength);
                                continue;
                            }
                            const int socketError = Platform::PlatformError::lastSocketErrorCode();
                            if (socketError == Platform::PlatformError::kWouldBlock || socketError == Platform::PlatformError::kInterrupted)
                            {
                                return false;
                            }
                            m_isClosedByPeer = true;
                            return true;
                        }
                        return true;
                    },
                    kWaitTimeout, kPollInterval)
                   && writtenLength == bytes.size();
        }

        /**
         * @brief 有界地读到「判定成立」或「通道已断」
         * @param isDone 判定体（读的是本对象已解出的状态）
         * @param timeout 等待上限
         * @return true 判定在时限内成立
         */
        bool pumpUntil(const std::function<bool()> &isDone, const std::chrono::milliseconds timeout)
        {
            static_cast<void>(Samples::waitUntil(
                    [this, &isDone]()
                    {
                        drainDecodedBytes();
                        if (isDone() || m_isClosedByPeer)
                        {
                            return true;
                        }
                        const ReadOutcome outcome = readOnce();
                        if (outcome == ReadOutcome::PeerClosed || outcome == ReadOutcome::Broken)
                        {
                            m_isClosedByPeer = true;
                            return true;
                        }
                        return false;
                    },
                    timeout, kPollInterval));
            drainDecodedBytes();
            return isDone();
        }

        /// 累计收到的全部明文字节（HTTP/1.1 那几步直接搜它）
        [[nodiscard]] const std::string &receivedText() const noexcept
        {
            return m_receivedText;
        }

        [[nodiscard]] bool sawPeerSettings() const noexcept
        {
            return m_sawPeerSettings;
        }

        [[nodiscard]] bool sawSettingsAcknowledgement() const noexcept
        {
            return m_sawSettingsAcknowledgement;
        }

        /// 对端 SETTINGS 里通告过的参数；没通告过时返回空
        [[nodiscard]] std::optional<std::uint32_t> peerSetting(const Net::Http2SettingIdentifier identifier) const
        {
            const auto found = m_peerSettings.find(identifier);
            if (found == m_peerSettings.end())
            {
                return std::nullopt;
            }
            return found->second;
        }

        [[nodiscard]] bool sawGoAway() const noexcept
        {
            return m_sawGoAway;
        }

        [[nodiscard]] std::uint32_t goAwayLastStreamId() const noexcept
        {
            return m_goAwayLastStreamId;
        }

        [[nodiscard]] Net::Http2ErrorCode goAwayErrorCode() const noexcept
        {
            return m_goAwayErrorCode;
        }

        /// GOAWAY 里带的调试文本（服务端写的中文原因，排查时正是要看它）
        [[nodiscard]] const std::string &goAwayDebugText() const noexcept
        {
            return m_goAwayDebugText;
        }

        /// 某条流上收到的 RST_STREAM 错误码；没被复位过时返回空
        [[nodiscard]] std::optional<Net::Http2ErrorCode> resetStreamCode(const std::uint32_t streamId) const
        {
            const auto found = m_resetStreamCodes.find(streamId);
            return found == m_resetStreamCodes.end() ? std::nullopt : std::optional<Net::Http2ErrorCode>{found->second};
        }

        [[nodiscard]] bool hasResponseHeaderBlock(const std::uint32_t streamId) const
        {
            const auto found = m_responseHeaderBlocks.find(streamId);
            return found != m_responseHeaderBlocks.end() && !found->second.empty();
        }

        /**
         * @brief 取某条流第 blockIndex 个响应头块里某个头的值
         * @param streamId 流号
         * @param name 头名（h2 上一律小写，伪头带 ':'）
         * @param blockIndex 第几个头块，从 0 起（100 之后还有 200 时用得上）
         * @return std::string 头值；取不到时为空串
         */
        [[nodiscard]] std::string responseHeaderValue(const std::uint32_t streamId, const std::string_view name,
                                                      const std::size_t blockIndex = 0) const
        {
            const auto found = m_responseHeaderBlocks.find(streamId);
            if (found == m_responseHeaderBlocks.end() || blockIndex >= found->second.size())
            {
                return {};
            }
            for (const Net::HpackHeaderField &field: found->second[blockIndex])
            {
                if (field.name == name)
                {
                    return field.value;
                }
            }
            return {};
        }

        /// 某条流上收到的全部 DATA 正文（按到达顺序拼接）
        [[nodiscard]] std::string responsePayload(const std::uint32_t streamId) const
        {
            const auto found = m_responseBodies.find(streamId);
            return found == m_responseBodies.end() ? std::string{} : found->second;
        }

        /// 该流上是否出现过 END_STREAM（响应到此为止）
        [[nodiscard]] bool hasEndStream(const std::uint32_t streamId) const
        {
            return m_endStreamSeen.find(streamId) != m_endStreamSeen.end();
        }

        /// 该流是否已经「头块 + END_STREAM」两件事齐了（响应到此为止）
        [[nodiscard]] bool isResponseCompleteOnStream(const std::uint32_t streamId) const
        {
            return hasResponseHeaderBlock(streamId) && hasEndStream(streamId);
        }

        /// 通道是否已断（对端关闭，或本地写出/读入判定为不可用）
        [[nodiscard]] bool isClosedByPeer() const noexcept
        {
            return m_isClosedByPeer;
        }

        void closeNow() noexcept
        {
            if (m_ssl != nullptr)
            {
                // 先尽力发出 close_notify：让服务端看到的是正常关闭，而不是半路断掉
                static_cast<void>(SSL_shutdown(m_ssl));
                SSL_free(m_ssl);
                m_ssl = nullptr;
            }
            if (m_context != nullptr)
            {
                SSL_CTX_free(m_context);
                m_context = nullptr;
            }
            Platform::FileDescriptor::close(m_descriptor);
            m_descriptor = Platform::FileDescriptor::kInvalid;
        }

    private:
        Http2LoopbackClient() = default;

        /// 连到 127.0.0.1:port 并转非阻塞：之后所有推进都靠有界轮询
        void connectTo(const std::uint16_t port)
        {
            const int descriptor = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (!Platform::FileDescriptor::isValid(descriptor))
            {
                m_handshakeFailureReason = "创建回环套接字失败";
                return;
            }
            sockaddr_in address{};
            address.sin_family      = AF_INET;
            address.sin_port        = htons(port);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::connect(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
            {
                m_handshakeFailureReason = "连接回环端口失败";
                Platform::FileDescriptor::close(descriptor);
                return;
            }
            Platform::FileDescriptor::setNonBlocking(descriptor);
            m_descriptor = descriptor;
        }

        /**
         * @brief 建客户端 SSL 对象：按需登记 ALPN、按需校验服务端证书
         * @param offeredAlpnProtocol 要登记进 ALPN 的协议名；空串表示不提供
         * @param trustedCertificateFile 非空时只信这一张自签证书，并按 127.0.0.1 校验名字
         */
        void installTls(const std::string_view offeredAlpnProtocol, const std::string &trustedCertificateFile)
        {
            m_context = SSL_CTX_new(TLS_client_method());
            if (m_context == nullptr)
            {
                m_handshakeFailureReason = describeChannelFailure("创建客户端 TLS 上下文");
                return;
            }
            if (!trustedCertificateFile.empty())
            {
                static_cast<void>(SSL_CTX_load_verify_locations(m_context, trustedCertificateFile.c_str(), nullptr));
                SSL_CTX_set_verify(m_context, SSL_VERIFY_PEER, nullptr);
                m_verifiesPeerCertificate = true;
            }
            m_ssl = SSL_new(m_context);
            if (m_ssl == nullptr || SSL_set_fd(m_ssl, m_descriptor) != 1)
            {
                m_handshakeFailureReason = describeChannelFailure("创建 SSL 对象并绑定描述符");
                return;
            }
            // 主机名校验：只看「链是受信的」不足以证明这张证书就是发给这台主机的
            if (m_verifiesPeerCertificate)
            {
                static_cast<void>(X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(m_ssl), "127.0.0.1"));
            }
            if (!offeredAlpnProtocol.empty())
            {
                // 线上格式是「长度前缀 + 协议名」；SSL_set_alpn_protos 返回 0 才是接受
                std::vector<unsigned char> wireFormat;
                wireFormat.push_back(static_cast<unsigned char>(offeredAlpnProtocol.size()));
                for (const char nameByte: offeredAlpnProtocol)
                {
                    wireFormat.push_back(static_cast<unsigned char>(nameByte));
                }
                m_isAlpnListAccepted = SSL_set_alpn_protos(m_ssl, wireFormat.data(), static_cast<unsigned int>(wireFormat.size())) == 0;
            }
        }

        /// 推进 SSL_connect 到有结论（成功 / 硬失败 / 到期）
        bool performHandshake()
        {
            if (m_ssl == nullptr)
            {
                return false;
            }
            bool isHardFailure = false;
            static_cast<void>(Samples::waitUntil(
                    [this, &isHardFailure]()
                    {
                        const int result = SSL_connect(m_ssl);
                        if (result == 1)
                        {
                            m_isHandshakeDone = true;
                            return true;
                        }
                        const int sslError = SSL_get_error(m_ssl, result);
                        if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE)
                        {
                            return false;
                        }
                        isHardFailure = true;
                        return true;
                    },
                    kWaitTimeout, kPollInterval));
            if (m_isHandshakeDone)
            {
                return true;
            }
            std::string reason = isHardFailure ? describeChannelFailure("TLS 握手") : "TLS 握手在时限内没走完";
            if (const std::string verifyText = certificateVerifyFailureText(); !verifyText.empty())
            {
                reason += "（证书校验：" + verifyText + "）";
            }
            m_handshakeFailureReason = reason;
            return false;
        }

        /// 把 OpenSSL 错误栈与系统 errno 折成一行可读原因
        std::string describeChannelFailure(const std::string_view action) const
        {
            std::string reason(action);
            reason += "失败";
            if (const unsigned long rawError = ERR_get_error(); rawError != 0)
            {
                reason += "：";
                reason += ERR_reason_error_string(rawError) != nullptr ? ERR_reason_error_string(rawError) : "无原因文本";
            } else if (const int socketError = Platform::PlatformError::lastSocketErrorCode(); socketError != 0)
            {
                reason += "：套接字错误码 " + std::to_string(socketError);
            }
            return reason;
        }

        ReadOutcome readOnce()
        {
            std::array<char, 4096> chunkStorage{};
            if (m_ssl != nullptr)
            {
                const int readLength = SSL_read(m_ssl, chunkStorage.data(), static_cast<int>(chunkStorage.size()));
                if (readLength > 0)
                {
                    appendReceivedBytes(chunkStorage.data(), static_cast<std::size_t>(readLength));
                    return ReadOutcome::Data;
                }
                const int sslError = SSL_get_error(m_ssl, readLength);
                if (sslError == SSL_ERROR_ZERO_RETURN)
                {
                    return ReadOutcome::PeerClosed;
                }
                if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE)
                {
                    return ReadOutcome::Idle;
                }
                // 对端没发 close_notify 就断开时是 SYSCALL + 0：按「已关闭」而不是「出错」处理
                return (sslError == SSL_ERROR_SYSCALL && readLength == 0) ? ReadOutcome::PeerClosed : ReadOutcome::Broken;
            }

            const ssize_t readLength = Platform::FileDescriptor::read(m_descriptor, chunkStorage.data(), chunkStorage.size());
            if (readLength > 0)
            {
                appendReceivedBytes(chunkStorage.data(), static_cast<std::size_t>(readLength));
                return ReadOutcome::Data;
            }
            if (readLength == 0)
            {
                return ReadOutcome::PeerClosed;
            }
            const int socketError = Platform::PlatformError::lastSocketErrorCode();
            if (socketError == Platform::PlatformError::kWouldBlock || socketError == Platform::PlatformError::kInterrupted)
            {
                return ReadOutcome::Idle;
            }
            return ReadOutcome::Broken;
        }

        void appendReceivedBytes(const char *data, const std::size_t length)
        {
            m_receivedText.append(data, length);
            m_pendingFrameBytes.append(data, length);
        }

        /// 把已收到的字节喂给生产帧解码器，产出的帧就地解释成状态
        void drainDecodedBytes()
        {
            if (m_hasFrameDecodeError)
            {
                // 已经解出过非法帧（HTTP/1.1 那几步就是这种情形）：不再喂字节，文本累计照旧可读
                m_pendingFrameBytes.clear();
                return;
            }
            std::size_t consumedByteCount = 0;
            while (consumedByteCount < m_pendingFrameBytes.size())
            {
                const Net::Http2FrameDecodeStatus status = m_frameDecoder.parse(m_pendingFrameBytes.data() + consumedByteCount,
                                                                                m_pendingFrameBytes.size() - consumedByteCount);
                if (status == Net::Http2FrameDecodeStatus::Error)
                {
                    m_hasFrameDecodeError = true;
                    LOG_INFO_FMT("示例客户端：帧解码失败，之后的字节只按文本累计。原因：{}", m_frameDecoder.errorMessage());
                    break;
                }
                if (status != Net::Http2FrameDecodeStatus::Frame)
                {
                    // NeedMore：半帧由解码器自己缓冲，下一批字节接着喂
                    consumedByteCount = m_pendingFrameBytes.size();
                    break;
                }
                consumedByteCount += m_frameDecoder.consumedByteCount();
                absorbFrame(m_frameDecoder.takeFrame());
            }
            m_pendingFrameBytes.erase(0, consumedByteCount);
        }

        void absorbFrame(const Net::Http2Frame &frame)
        {
            const std::uint32_t streamId = frame.header.streamId;
            const std::uint8_t  flags    = frame.header.flags;
            switch (frame.header.type)
            {
                case Net::Http2FrameType::Settings:
                    absorbSettings(frame, flags);
                    break;
                case Net::Http2FrameType::GoAway:
                    absorbGoAway(frame);
                    break;
                case Net::Http2FrameType::RstStream:
                    absorbRstStream(frame, streamId);
                    break;
                case Net::Http2FrameType::Headers:
                    appendHeaderFragment(streamId, frame.payload, (flags & Net::kHttp2FlagEndHeaders) != 0,
                                         (flags & Net::kHttp2FlagEndStream) != 0);
                    break;
                case Net::Http2FrameType::Continuation:
                    appendHeaderFragment(streamId, frame.payload, (flags & Net::kHttp2FlagEndHeaders) != 0, false);
                    break;
                case Net::Http2FrameType::Data:
                    if ((flags & Net::kHttp2FlagEndStream) != 0)
                    {
                        m_endStreamSeen.insert(streamId);
                    }
                    m_responseBodies[streamId] += frame.payload;
                    break;
                default:
                    // PING/PRIORITY/WINDOW_UPDATE：本示例不主动探测，收到也不改变判定
                    break;
            }
        }

        void absorbSettings(const Net::Http2Frame &frame, const std::uint8_t flags)
        {
            if ((flags & Net::kHttp2FlagAcknowledge) != 0)
            {
                m_sawSettingsAcknowledgement = true;
                return;
            }
            Net::Http2SettingsPayload payload;
            std::string               errorText;
            if (!Net::parseHttp2SettingsPayload(frame, payload, &errorText))
            {
                LOG_ERROR_FMT("示例客户端：服务端 SETTINGS 解不开。原因：{}", errorText);
                return;
            }
            for (const Net::Http2Setting &setting: payload.parameters)
            {
                m_peerSettings.emplace(static_cast<Net::Http2SettingIdentifier>(setting.identifier), setting.value);
            }
            m_sawPeerSettings = true;
        }

        void absorbGoAway(const Net::Http2Frame &frame)
        {
            Net::Http2GoAwayPayload payload;
            std::string             errorText;
            if (!Net::parseHttp2GoAwayPayload(frame, payload, &errorText))
            {
                LOG_ERROR_FMT("示例客户端：GOAWAY 解不开。原因：{}", errorText);
                return;
            }
            m_sawGoAway            = true;
            m_goAwayLastStreamId   = payload.lastStreamId;
            m_goAwayErrorCode      = payload.errorCode;
            m_goAwayDebugText      = payload.debugData;
        }

        void absorbRstStream(const Net::Http2Frame &frame, const std::uint32_t streamId)
        {
            Net::Http2RstStreamPayload payload;
            std::string                errorText;
            if (!Net::parseHttp2RstStreamPayload(frame, payload, &errorText))
            {
                LOG_ERROR_FMT("示例客户端：RST_STREAM 解不开。原因：{}", errorText);
                return;
            }
            m_resetStreamCodes[streamId] = payload.errorCode;
        }

        /**
         * @brief 拼一个头块片段，END_HEADERS 时整块交给 HpackDecoder 解回
         * @details 解码器与编码器成对演进：同一条连接上的多个响应必须按到达顺序解，
         *          换一个全新解码器就会解不开第二条（动态表里的索引无人认得）
         */
        void appendHeaderFragment(const std::uint32_t streamId, const std::string_view fragment, const bool endHeaders,
                                  const bool endStream)
        {
            if (endStream)
            {
                m_endStreamSeen.insert(streamId);
            }
            std::string &partial = m_partialHeaderBlocks[streamId];
            partial.append(fragment);
            if (!endHeaders)
            {
                return;
            }
            std::vector<Net::HpackHeaderField> headerFields;
            std::string                        errorText;
            if (m_headerDecoder.decode(partial, headerFields, &errorText))
            {
                m_responseHeaderBlocks[streamId].push_back(std::move(headerFields));
            }
            else
            {
                LOG_ERROR_FMT("示例客户端：响应头块解不开。原因：{}", errorText);
            }
            partial.clear();
        }

        Platform::Socket::Initialization m_socketInitialization; ///< 保证 Winsock 在本对象存活期间保持初始化

        int   m_descriptor{Platform::FileDescriptor::kInvalid}; ///< 底层描述符（非阻塞）
        SSL  *m_ssl{nullptr};                                   ///< TLS 对象；空表示明文连接
        SSL_CTX *m_context{nullptr};                            ///< 客户端上下文（本对象持有）
        bool  m_isAlpnListAccepted{false};                      ///< ALPN 列表是否被客户端侧接受
        bool  m_verifiesPeerCertificate{false};                 ///< 是否启用了服务端证书校验
        bool  m_isHandshakeDone{false};                         ///< TLS 握手是否走完
        bool  m_isClosedByPeer{false};                          ///< 通道是否已断
        std::string m_handshakeFailureReason;                   ///< 失败原因（握手或连接）

        std::string           m_receivedText;       ///< 累计收到的明文字节（HTTP/1.1 步骤用）
        std::string           m_pendingFrameBytes;  ///< 还没喂给解码器的字节
        Net::Http2FrameDecoder m_frameDecoder;      ///< 生产帧解码器
        Net::HpackDecoder      m_headerDecoder;     ///< 生产头块解码器（整条连接共用一份）
        bool                   m_hasFrameDecodeError{false}; ///< 帧解码是否已失败

        std::map<std::uint32_t, std::string>                          m_partialHeaderBlocks;   ///< 拼装中的头块
        std::map<std::uint32_t, std::vector<std::vector<Net::HpackHeaderField>>> m_responseHeaderBlocks; ///< 各流的响应头块
        std::map<std::uint32_t, std::string>                          m_responseBodies;        ///< 各流的正文
        std::map<std::uint32_t, Net::Http2ErrorCode>                  m_resetStreamCodes;      ///< 各流收到的 RST_STREAM
        std::map<Net::Http2SettingIdentifier, std::uint32_t>          m_peerSettings;          ///< 对端 SETTINGS 记账
        std::set<std::uint32_t>                                         m_endStreamSeen;         ///< 已收到 END_STREAM 的流号
        bool                                                          m_sawPeerSettings{false};            ///< 是否收到服务端 SETTINGS
        bool                                                          m_sawSettingsAcknowledgement{false}; ///< 是否看到对我们 SETTINGS 的 ACK
        bool                                                          m_sawGoAway{false};                    ///< 是否收到 GOAWAY
        std::uint32_t                                                 m_goAwayLastStreamId{0};             ///< GOAWAY 带的最后流号
        Net::Http2ErrorCode                                           m_goAwayErrorCode{Net::Http2ErrorCode::NoError}; ///< GOAWAY 错误码
        std::string                                                   m_goAwayDebugText{};                   ///< GOAWAY 的调试数据
    };

    /**
     * @brief 走完 h2 连接前奏：发前奏与自己的 SETTINGS → 等服务端 SETTINGS → 回 ACK 并放行窗口
     * @details 明文与 TLS 两条传输走同一段字节：差别只在传输通道，不在这套协商次序上
     * @param client 已就绪的连接
     * @return bool 前奏与 SETTINGS 是否双向交换完成（含服务端对我们 SETTINGS 的 ACK）
     */
    bool performHttp2Handshake(Http2LoopbackClient &client)
    {
        if (!client.sendBytes(std::string(Net::kHttp2ConnectionPreface) + makeClientSettingsFrame()))
        {
            LOG_ERROR("示例客户端：前奏与 SETTINGS 没能写出去");
            return false;
        }
        const bool gotSettings = client.pumpUntil([&client]()
                                                 {
                                                     return client.sawPeerSettings();
                                                 },
                                                 kWaitTimeout);
        if (!gotSettings)
        {
            LOG_ERROR("示例客户端：没在时限内收到服务端的初始 SETTINGS");
            return false;
        }
        if (!client.sendBytes(makeSettingsAckFrame() + makeConnectionWindowUpdateFrame()))
        {
            LOG_ERROR("示例客户端：SETTINGS ACK 没能写出去");
            return false;
        }
        // 服务端的 ACK 排在自己的 SETTINGS 之后：等到它才算「协商双向完成」
        return client.pumpUntil([&client]()
                               {
                                   return client.sawSettingsAcknowledgement();
                               },
                               kWaitTimeout);
    }

    /// @return bool 一个 request-id 是否是服务器自动生成的形态
    bool looksLikeGeneratedRequestId(const std::string_view requestId)
    {
        if (requestId.size() != kGeneratedRequestIdLength || requestId[kRequestIdSeparatorIndex] != '-')
        {
            return false;
        }
        for (std::size_t index = 0; index < requestId.size(); ++index)
        {
            if (index == kRequestIdSeparatorIndex)
            {
                continue;
            }
            const char character = requestId[index];
            const bool isLowerHexDigit = (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
            if (!isLowerHexDigit)
            {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 从 Prometheus 文本里取一条计数行的取值
     * @param metricsBody /metrics 的响应正文
     * @param linePrefix 值行的前缀（含标签，如 asyn_http_responses_total{status_class="2xx"}）
     * @return std::optional<std::uint64_t> 取到的数值；没有这一行时为空
     */
    std::optional<std::uint64_t> readPrometheusCounter(const std::string &metricsBody, const std::string &linePrefix)
    {
        std::size_t lineStart = 0;
        while (lineStart <= metricsBody.size())
        {
            const std::size_t lineEnd = metricsBody.find('\n', lineStart);
            const std::size_t  stopAt  = lineEnd == std::string::npos ? metricsBody.size() : lineEnd;
            const std::string_view line(metricsBody.data() + lineStart, stopAt - lineStart);
            if (line.rfind(linePrefix, 0) == 0)
            {
                std::string_view remainder = line.substr(linePrefix.size());
                const std::size_t firstDigit = remainder.find_first_of("0123456789");
                if (firstDigit != std::string_view::npos)
                {
                    remainder = remainder.substr(firstDigit);
                    const std::size_t lastDigit = remainder.find_last_of("0123456789");
                    try
                    {
                        return std::stoull(std::string(remainder.substr(0, lastDigit + 1)));
                    } catch (const std::exception &)
                    {
                        return std::nullopt;
                    }
                }
            }
            if (lineEnd == std::string::npos)
            {
                break;
            }
            lineStart = lineEnd + 1;
        }
        return std::nullopt;
    }

    /**
     * @brief 一台跑在自己事件循环上的服务器：装配、起服、优雅关停与收尾
     * @details 循环在独立线程里转，客户端动作留在主线程：与真实部署里「服务线程 + 运维线程」同形。
     */
    class RunningServerHost
    {
    public:
        /// 在指定循环与地址上造服务器：路由与各项开关都在这一步里落定（必须在 start() 之前）
        using ServerFactory = std::function<std::unique_ptr<Net::TcpServer>(Core::EventLoop &, const Core::InetAddress &)>;

        RunningServerHost(const RunningServerHost &) = delete;
        RunningServerHost &operator=(const RunningServerHost &) = delete;

        explicit RunningServerHost(const std::uint16_t port, const ServerFactory &factory)
        {
            try
            {
                const std::optional<Core::InetAddress> address = Core::InetAddress::resolve("127.0.0.1", port);
                if (!address.has_value())
                {
                    m_failureReason = "解析回环地址失败，端口 " + std::to_string(port);
                }
                else
                {
                    m_server        = factory(m_loop, *address);
                    m_acceptTask.emplace(driveStartTask(*m_server, m_startFailureReason, m_startThrew));
                    m_loop.scheduler().schedule(m_acceptTask->handle());
                }
            } catch (const Base::Exception &failure)
            {
                m_server.reset();
                m_failureReason = failure.what();
            } catch (const std::exception &failure)
            {
                m_server.reset();
                m_failureReason = failure.what();
            }
            m_loopThread = std::thread([this]()
                                      {
                                          m_loop.run();
                                      });
        }

        ~RunningServerHost()
        {
            // 顺序要紧：先让循环线程停手并退出，再收尾服务器（close() 会销毁挂起的协程帧）
            m_loop.stop();
            if (m_loopThread.joinable())
            {
                m_loopThread.join();
            }
            if (m_server != nullptr)
            {
                m_server->close();
            }
        }

        [[nodiscard]] bool isBuilt() const noexcept
        {
            return m_server != nullptr;
        }

        [[nodiscard]] Net::TcpServer &server() noexcept
        {
            return *m_server;
        }

        [[nodiscard]] const std::string &failureReason() const noexcept
        {
            return m_failureReason;
        }

        [[nodiscard]] bool startThrew() const noexcept
        {
            return m_startThrew.load(std::memory_order_acquire);
        }

        [[nodiscard]] const std::string &startFailureReason() const noexcept
        {
            return m_startFailureReason;
        }

        /// 等到服务器真的进入接受循环
        [[nodiscard]] bool awaitAcceptingLoop() const
        {
            return Samples::waitUntil(
                    [this]()
                    {
                        return m_server != nullptr && m_server->isRunning();
                    },
                    kWaitTimeout, std::chrono::milliseconds{5});
        }

        /**
         * @brief 在服务器所属的循环线程上投递「停止接受 + 排空在途请求」
         * @param drainTimeout 交给 drain 的最长等待时长
         * @return true drain 在自己的期限内跑完
         */
        bool requestGracefulDrain(const std::chrono::milliseconds drainTimeout)
        {
            if (m_server == nullptr)
            {
                return false;
            }
            m_drainFinished.store(false, std::memory_order_release);
            Core::Task<> drainTask = drainOnLoopTask(*m_server, drainTimeout, m_drainFinished);
            m_loop.scheduler().scheduleRemote(drainTask.handle());
            // 帧必须留到跑完：本向量持有到本对象析构（届时循环线程已经退出）
            m_shutdownTasks.push_back(std::move(drainTask));
            return Samples::waitUntil(
                    [this]()
                    {
                        return m_drainFinished.load(std::memory_order_acquire);
                    },
                    drainTimeout * 4 + kWaitTimeout, std::chrono::milliseconds{5});
        }

    private:
        /**
         * @brief 把 start() 包一层：它抛的异常要留下原因，而不是把循环线程带崩
         * @param server 目标服务器
         * @param startFailureReason 输出：异常文本
         * @param startThrew 输出：是否抛过（最后落定，读它的线程据此再看原因文本）
         * @return Core::Task<> 协程
         */
        static Core::Task<> driveStartTask(Net::TcpServer &server, std::string &startFailureReason, std::atomic<bool> &startThrew)
        {
            try
            {
                co_await server.start();
            } catch (const Base::Exception &failure)
            {
                startFailureReason = failure.what();
                startThrew.store(true, std::memory_order_release);
            } catch (const std::exception &failure)
            {
                startFailureReason = failure.what();
                startThrew.store(true, std::memory_order_release);
            }
            co_return;
        }

        /**
         * @brief 投递给循环线程的优雅关停协程：stop() 与 drain() 都只能在那个线程上跑
         * @param server 目标服务器
         * @param drainTimeout 交给 drain 的最长等待时长
         * @param drainFinished 输出：drain 已返回
         * @return Core::Task<> 协程
         */
        static Core::Task<> drainOnLoopTask(Net::TcpServer &server, const std::chrono::milliseconds drainTimeout,
                                            std::atomic<bool> &drainFinished)
        {
            server.stop();
            co_await server.drain(drainTimeout);
            drainFinished.store(true, std::memory_order_release);
            co_return;
        }

        Core::EventLoop                m_loop;            ///< 承载本服务器的循环
        std::unique_ptr<Net::TcpServer> m_server;         ///< 服务器本体；构造失败时为空
        std::optional<Core::Task<>>    m_acceptTask;      ///< 接受协程的帧
        std::vector<Core::Task<>>      m_shutdownTasks;   ///< 关停协程的帧
        std::thread                    m_loopThread;      ///< 跑 run() 的线程，最后构造、最先析构
        std::atomic<bool>              m_startThrew{false};    ///< start() 是否以异常收场（发布位最后写）
        std::atomic<bool>              m_drainFinished{false}; ///< drain 是否已返回
        std::string                    m_startFailureReason;   ///< start() 的异常文本
        std::string                    m_failureReason;        ///< 装配阶段的失败原因
    };

    /**
     * @brief 放宽三项时限：示例只观察协议行为，不该被空闲/读写清扫中途收口
     * @return Net::HttpServerLimits 一份「超时都很长」的限额
     */
    Net::HttpServerLimits makeLongTimeoutLimits()
    {
        Net::HttpServerLimits limits;
        limits.idleTimeout                    = std::chrono::seconds{10};
        limits.readTimeout                    = std::chrono::seconds{10};
        limits.writeTimeout                   = std::chrono::seconds{10};
        limits.settingsAcknowledgementTimeout = std::chrono::seconds{10};
        return limits;
    }

    /**
     * @brief 用框架的出站客户端发一次 GET（自建循环，跑完即停）
     * @param url 目标地址
     * @param requestTimeout 本次请求的整体时限
     * @return std::unique_ptr<Net::HttpClientResponse> 响应；失败或抛异常都返回空
     */
    std::unique_ptr<Net::HttpClientResponse> requestWithHttpClient(const std::string &url, const std::chrono::milliseconds requestTimeout)
    {
        Core::EventLoop                     loop;
        std::unique_ptr<Net::HttpClientResponse> result;
        // 惰性协程的帧记住的是闭包对象的地址：闭包必须先落到具名变量上再调用
        const auto requestBody = [&loop, &result, &url, requestTimeout]() -> Core::Task<>
        {
            try
            {
                result = co_await Net::HttpClient::get(loop, url, requestTimeout);
            } catch (const Base::Exception &failure)
            {
                LOG_ERROR_FMT("HttpClient 抛出框架异常，本步按失败处理。原因：{}", failure.what());
            } catch (const std::exception &failure)
            {
                LOG_ERROR_FMT("HttpClient 抛出非框架异常，本步按失败处理。原因：{}", failure.what());
            }
            loop.stop();
            co_return;
        };
        Core::Task<> request = requestBody();
        if (!request.isReady())
        {
            loop.scheduler().schedule(request.handle());
        }
        loop.run();
        return result;
    }

    /**
     * @brief 给一台服务器装好本示例要用的业务路由
     * @param router 目标路由器
     */
    void installSampleRoutes(Net::Router &router)
    {
        router.get("/hello", [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<>
        {
            response.setBody("served-hello");
            co_return;
        });
        router.post("/echo", [](Net::HttpRequest &request, Net::HttpResponse &response) -> Core::Task<>
        {
            // 回显正文长度：只有收全了才对得上，超限路径则根本走不到这里
            response.setBody(std::to_string(request.body().size()));
            co_return;
        });
        for (std::size_t streamIndex = 0; streamIndex < kMultiplexedStreamCount; ++streamIndex)
        {
            // 每条流要一个不同正文：串流（把 A 的正文发给 B）是多路复用最容易犯又最难察觉的错
            const std::size_t index = streamIndex;
            router.get("/mux/" + std::to_string(index), [index](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<>
            {
                response.setBody("mux-answer-" + std::to_string(index));
                co_return;
            });
        }
    }

    /**
     * @brief 追加一条「占住处理器一段时间」的路由：给优雅关停留下一件货真价实的在途工作
     * @param router 目标路由器
     * @param loopForTimer 等待定时器挂在哪个循环上（处理器就跑在那个线程上，定时器必须同循环）
     * @param handlerEntered 输出：处理器是否已经跑起来（主线程据此再发起 drain，否则「等到了在途」是空的）
     */
    void installInflightRoute(Net::Router &router, Core::EventLoop &loopForTimer, std::atomic<bool> &handlerEntered)
    {
        router.get("/slow", [&loopForTimer, &handlerEntered](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<>
        {
            handlerEntered.store(true, std::memory_order_release);
            Core::Timer holdTimer(loopForTimer);
            co_await holdTimer.waitFor(kInflightWorkDuration);
            response.setBody("slow-finished");
            co_return;
        });
    }

    // ============================================================================
    // 一. TLS 起服 + ALPN 协商 h2 的端到端往返
    // ============================================================================

    void demonstrateTlsAlpnHttp2(const TlsMaterial &material, const std::uint16_t port)
    {
        auto &samples = Samples::checklist();
        // 具体类型指针：stats()、parserLimits() 这类观测口在 HttpsServer 上，基类 TcpServer 没有
        Net::HttpsServer *tlsServer = nullptr;

        RunningServerHost host(port,
                               [&material, &tlsServer](Core::EventLoop &loop, const Core::InetAddress &address) -> std::unique_ptr<Net::TcpServer>
                               {
                                   auto server = std::make_unique<Net::HttpsServer>(loop, address, material.certificateFile.string(),
                                                                                    material.keyFile.string());
                                   tlsServer = server.get();
                                   server->setLimits(makeLongTimeoutLimits());
                                   server->setIdleCheckInterval(std::chrono::milliseconds{50});
                                   installSampleRoutes(server->router());
                                   // 指标与健康端点默认不开：本示例要在 h2 上抓到它们，就得在这里显式打开
                                   server->enableMetricsEndpoint();
                                   server->enableHealthEndpoint();
                                   return server;
                               });

        const bool isServing = host.isBuilt() && host.awaitAcceptingLoop();
        samples.check(isServing && !host.startThrew(),
                      isServing ? "HttpsServer 用夹具证书在 samplePort 上进入接受循环"
                                : ("HttpsServer 没能起来：" + (host.isBuilt() ? host.startFailureReason() : host.failureReason())));
        if (!isServing)
        {
            return;
        }

        // —— 1. 客户端必须拒掉「不受信的服务端证书」，且拒因看得见 ——
        const std::unique_ptr<Http2LoopbackClient> untrustedClient =
                Http2LoopbackClient::openOverTls(port, "h2", material.foreignCertificateFile.string());
        LOG_INFO_FMT("不受信客户端的握手结论：完成={}，原因={}", untrustedClient->isHandshakeComplete(),
                     untrustedClient->handshakeFailureReason());
        samples.check(!untrustedClient->isHandshakeComplete()
                              && untrustedClient->certificateVerifyFailureText().find("certificate") != std::string::npos,
                      "只信另一张自签根的客户端在握手期就被拒，原因文本里带着证书校验的说明");

        // —— 2. 反向对照：同一台服务器、按回环 IP 校验名字，握手就该成 ——
        //（缺了这条，上一条可能只是「TLS 整条路都不通」而不是「校验真的在拦」）
        const std::unique_ptr<Http2LoopbackClient> trustedClient = Http2LoopbackClient::openOverTls(port, "h2", material.certificateFile.string());
        LOG_INFO_FMT("受信客户端的握手结论：完成={}，原因={}", trustedClient->isHandshakeComplete(), trustedClient->handshakeFailureReason());
        samples.check(trustedClient->isHandshakeComplete(), "把服务端那张夹具证书当受信根、并按 127.0.0.1 校验名字的客户端握手成功");

        // —— 3. ALPN 协商出 h2 ——
        const std::unique_ptr<Http2LoopbackClient> client = Http2LoopbackClient::openOverTls(port, "h2", {});
        samples.check(client->isAlpnListAccepted() && client->isHandshakeComplete()
                              && client->selectedApplicationProtocol() == "h2",
                      "ALPN 提 h2 时两端协商出的协议名就是 h2");
        samples.check(performHttp2Handshake(*client), "h2 连接前奏与 SETTINGS 双向交换完成（服务端也确认了我们的 SETTINGS）");

        const std::optional<std::uint32_t> advertisedStreams = client->peerSetting(Net::Http2SettingIdentifier::MaxConcurrentStreams);
        const std::optional<std::uint32_t> advertisedHeaderSize = client->peerSetting(Net::Http2SettingIdentifier::MaxHeaderListSize);
        const std::optional<std::uint32_t> advertisedFrameSize = client->peerSetting(Net::Http2SettingIdentifier::MaxFrameSize);
        const std::optional<std::uint32_t> advertisedPush = client->peerSetting(Net::Http2SettingIdentifier::EnablePush);
        const std::optional<std::uint32_t> advertisedWindow = client->peerSetting(Net::Http2SettingIdentifier::InitialWindowSize);
        LOG_INFO_FMT("服务端通告：并发流上限={}，头列表上限={}，帧上限={}，允许推送={}，初始窗口={}",
                     advertisedStreams.value_or(0), advertisedHeaderSize.value_or(0), advertisedFrameSize.value_or(0),
                     advertisedPush.value_or(0), advertisedWindow.value_or(0));
        // 这些取值出自 Http2ConnectionConfiguration，公开 API 上没有改它们的入口：这里钉住「通告与实现内定值一致」
        samples.check(advertisedStreams.has_value() && advertisedStreams.value() == 100U && advertisedHeaderSize.has_value()
                              && advertisedHeaderSize.value() == 16U * 1024U && advertisedFrameSize.has_value()
                              && advertisedFrameSize.value() == Net::kHttp2DefaultMaximumFrameSize && advertisedPush.has_value()
                              && advertisedPush.value() == 0U && advertisedWindow.has_value()
                              && advertisedWindow.value() == Net::kHttp2InitialWindowSizeByteCount,
                      "服务端的 SETTINGS 如实通告了本端上限（并发 100 条流、头列表 16 KiB、帧 16 KiB、不推送）");

        // —— 4. 一条完整往返：GET /hello → 200 + 正文 + END_STREAM ——
        static_cast<void>(client->sendBytes(makeRequestHeadersFrame(1U, makeGetRequestHeaderBlock("/hello", true), true)));
        const bool answered = client->pumpUntil(
                [&client]()
                {
                    return client->isResponseCompleteOnStream(1U);
                },
                kWaitTimeout);
        samples.check(answered && client->responseHeaderValue(1U, ":status") == "200" && client->responsePayload(1U) == "served-hello",
                      "TLS 上的 h2 GET 拿到 200 与正确正文，末片带 END_STREAM");

        const std::string generatedRequestId = client->responseHeaderValue(1U, std::string_view(Net::kRequestIdHeaderName));
        samples.check(looksLikeGeneratedRequestId(generatedRequestId),
                      "h2 响应带形态合法的 x-request-id（4 位十六进制前缀 + '-' + 16 位十六进制序号）");

        // —— 5. 客户端自带的 request-id 原样回显 ——
        const std::string clientRequestId = "h2-sample-trace-id";
        static_cast<void>(client->sendBytes(makeRequestHeadersFrame(
                3U, makeGetRequestHeaderBlock("/hello", true, {{std::string(Net::kRequestIdHeaderName), clientRequestId}}), true)));
        static_cast<void>(client->pumpUntil(
                [&client]()
                {
                    return client->isResponseCompleteOnStream(3U);
                },
                kWaitTimeout));
        samples.check(client->responseHeaderValue(3U, std::string_view(Net::kRequestIdHeaderName)) == clientRequestId,
                      "客户端自带的合法 request-id 在 h2 上被原样回显，没被服务器换掉");

        // —— 6. 多路复用：一条连接上并发四条流，各自拿到属于自己的响应 ——
        std::string multiplexedBytes;
        for (std::size_t streamIndex = 0; streamIndex < kMultiplexedStreamCount; ++streamIndex)
        {
            const std::uint32_t streamId = static_cast<std::uint32_t>(5U + streamIndex * 2U);
            multiplexedBytes += makeRequestHeadersFrame(streamId, makeGetRequestHeaderBlock("/mux/" + std::to_string(streamIndex), true), true);
        }
        static_cast<void>(client->sendBytes(multiplexedBytes));
        static_cast<void>(client->pumpUntil(
                [&client]()
                {
                    for (std::size_t streamIndex = 0; streamIndex < kMultiplexedStreamCount; ++streamIndex)
                    {
                        const std::uint32_t streamId = static_cast<std::uint32_t>(5U + streamIndex * 2U);
                        if (!client->isResponseCompleteOnStream(streamId))
                        {
                            return false;
                        }
                    }
                    return true;
                },
                kWaitTimeout));
        bool isMultiplexingCorrect = true;
        for (std::size_t streamIndex = 0; streamIndex < kMultiplexedStreamCount; ++streamIndex)
        {
            const std::uint32_t streamId = static_cast<std::uint32_t>(5U + streamIndex * 2U);
            isMultiplexingCorrect        = isMultiplexingCorrect && client->responsePayload(streamId) == "mux-answer-" + std::to_string(streamIndex)
                                    && client->responseHeaderValue(streamId, ":status") == "200";
        }
        samples.check(isMultiplexingCorrect, "一条 TLS 连接上并发四条流各得其所：流号互不相同、正文与请求一一对应");

        // —— 7. request-id/指标/健康端点这套服务端功能在 h2 上照常可用 ——
        static_cast<void>(client->sendBytes(makeRequestHeadersFrame(13U, makeGetRequestHeaderBlock("/healthz", true), true)));
        static_cast<void>(client->pumpUntil(
                [&client]()
                {
                    return client->isResponseCompleteOnStream(13U);
                },
                kWaitTimeout));
        samples.check(client->responseHeaderValue(13U, ":status") == "200" && client->responsePayload(13U) == std::string{Net::kHealthCheckResponseBody},
                      "h2 上的 /healthz 回的是与 h1 同一份固定正文（端点是普通路由，与协议无关）");

        static_cast<void>(client->sendBytes(makeRequestHeadersFrame(15U, makeGetRequestHeaderBlock("/metrics", true), true)));
        static_cast<void>(client->pumpUntil(
                [&client]()
                {
                    return client->isResponseCompleteOnStream(15U);
                },
                kWaitTimeout));
        const std::optional<std::uint64_t> requestCount = readPrometheusCounter(client->responsePayload(15U), "asyn_http_requests_total");
        // 抓取这一刻已派发的请求：/hello 两条 + /mux 四条 + /healthz + /metrics 自己 = 8（计数在派发前落账）
        LOG_INFO_FMT("第一次抓 /metrics 读到请求计数 {}", requestCount.has_value() ? std::to_string(requestCount.value()) : "<没有这一行>");
        samples.check(requestCount.has_value() && requestCount.value() >= 8U, "h2 上抓 /metrics 能看到 TLS 路径自己那份请求计数");

        static_cast<void>(client->sendBytes(makeRequestHeadersFrame(17U, makeGetRequestHeaderBlock("/metrics", true), true)));
        static_cast<void>(client->pumpUntil(
                [&client]()
                {
                    return client->isResponseCompleteOnStream(17U);
                },
                kWaitTimeout));
        const std::optional<std::uint64_t> responseCount =
                readPrometheusCounter(client->responsePayload(17U), "asyn_http_responses_total{status_class=\"2xx\"}");
        // 第二次抓取时前八条响应都已落账（本条自己的响应还没发完，故不计）
        LOG_INFO_FMT("第二次抓 /metrics 读到 2xx 响应计数 {}", responseCount.has_value() ? std::to_string(responseCount.value()) : "<没有这一行>");
        samples.check(responseCount.has_value() && responseCount.value() >= 8U, "第二次抓 /metrics 时状态码类计数已落账，与 stats() 同一份口径");

        const Net::HttpServerStats servedStats = tlsServer->stats();
        // 此刻这台服务器已派发九条请求（两条 /hello、四条 /mux、/healthz、两次 /metrics），其中八条的响应已落账
        LOG_INFO_FMT("stats() 快照：请求 {} 条，2xx {} 条，协议错误 {} 条", servedStats.totalRequestCount, servedStats.status2xxCount,
                     servedStats.badRequestCount);
        samples.check(servedStats.totalRequestCount >= 9U && servedStats.status2xxCount >= 8U && servedStats.badRequestCount == 0U,
                      "服务器侧 stats() 快照与线上观测量同向增长，且没把正常请求记成协议错误");

        // —— 8. ALPN 只提 http/1.1：同一条 TLS 连接要交回 HTTP/1.1 事务循环 ——
        const std::unique_ptr<Http2LoopbackClient> legacyClient = Http2LoopbackClient::openOverTls(port, "http/1.1", {});
        bool isLegacyServed = false;
        if (legacyClient->isHandshakeComplete() && legacyClient->selectedApplicationProtocol() == "http/1.1")
        {
            static_cast<void>(legacyClient->sendBytes("GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
            static_cast<void>(legacyClient->pumpUntil(
                    [&legacyClient]()
                    {
                        return legacyClient->receivedText().find("served-hello") != std::string::npos;
                    },
                    kWaitTimeout));
            isLegacyServed = legacyClient->receivedText().find("HTTP/1.1 200") != std::string::npos;
        }
        else
        {
            LOG_ERROR_FMT("ALPN 提 http/1.1 的客户端没能协商成功：{}", legacyClient->handshakeFailureReason());
        }
        samples.check(isLegacyServed, "ALPN 只提 http/1.1 时同一条 TLS 连接按 HTTP/1.1 服务，仍拿到 200 与正文");

        // —— 9. 框架自带客户端的失败面：一次请求一条连接、不登记 ALPN、只认系统 CA ——
        samples.check(runGuarded("HttpClient 直连自签服务端",
                                 [&port]()
                                 {
                                     const std::string url = "https://127.0.0.1:" + std::to_string(port) + "/hello";
                                     const std::unique_ptr<Net::HttpClientResponse> response =
                                             requestWithHttpClient(url, std::chrono::milliseconds{3000});
                                     if (response != nullptr)
                                     {
                                         LOG_ERROR_FMT("HttpClient 竟然拿到了 {}，自签证书的链校验没生效", response->statusCode);
                                     }
                                     return response == nullptr;
                                 }),
                      "Net::HttpClient 直连自签服务端按契约返回空响应（链不受信就失败，不静默降级）");
    }

    // ============================================================================
    // 二. TLS 上可达的解析上限：h1 回落下 431，h2 路径上 413
    // ============================================================================

    void demonstrateParserLimits(const TlsMaterial &material, const std::uint16_t port)
    {
        auto &samples   = Samples::checklist();
        Net::HttpsServer *limitsServer = nullptr;

        RunningServerHost host(port,
                               [&material, &limitsServer](Core::EventLoop &loop, const Core::InetAddress &address) -> std::unique_ptr<Net::TcpServer>
                               {
                                   auto server = std::make_unique<Net::HttpsServer>(loop, address, material.certificateFile.string(),
                                                                                    material.keyFile.string());
                                   limitsServer = server.get();
                                   server->setLimits(makeLongTimeoutLimits());
                                   server->setIdleCheckInterval(std::chrono::milliseconds{50});
                                   Net::HttpParserLimits parserLimits;
                                   parserLimits.maximumUriLength              = 64; // 请求行整行上限由它推出
                                   parserLimits.maximumHeaderFieldValueLength = 32;
                                   parserLimits.maximumBodySize               = 16;
                                   server->setParserLimits(parserLimits);
                                   installSampleRoutes(server->router());
                                   return server;
                               });
        if (!host.isBuilt() || !host.awaitAcceptingLoop())
        {
            samples.check(false, "限额服务器没能起来：" + (host.isBuilt() ? host.startFailureReason() : host.failureReason()));
            return;
        }
        samples.check(limitsServer->parserLimits().maximumHeaderFieldValueLength == 32U && limitsServer->parserLimits().maximumUriLength == 64U
                              && limitsServer->parserLimits().maximumBodySize == 16U,
                      "setParserLimits() 的三项上限都读得回来（会话按这一份判定，而不是默认值）");

        // 不提 ALPN 的客户端走 HTTP/1.1 路径：这里才看得到 431
        const std::unique_ptr<Http2LoopbackClient> client = Http2LoopbackClient::openOverTls(port, {}, {});
        if (!client->isHandshakeComplete())
        {
            samples.check(false, "限额服务器上的 TLS 连接没建成：" + client->handshakeFailureReason());
            return;
        }

        static_cast<void>(client->sendBytes("GET /hello HTTP/1.1\r\nHost: localhost\r\nx-blob: " + std::string(64, 'a')
                                            + "\r\nConnection: close\r\n\r\n"));
        static_cast<void>(client->pumpUntil(
                [&client]()
                {
                    return client->receivedText().find("HTTP/1.1 431") != std::string::npos || client->isClosedByPeer();
                },
                kWaitTimeout));
        const std::string &oversizeHeaderText = client->receivedText();
        samples.check(oversizeHeaderText.find("HTTP/1.1 431") != std::string::npos && oversizeHeaderText.find("HTTP/1.1 400") == std::string::npos,
                      "单个头部值越界回 431（体量越界）而不是 400（报文非法）");

        const std::unique_ptr<Http2LoopbackClient> longTargetClient = Http2LoopbackClient::openOverTls(port, {}, {});
        static_cast<void>(longTargetClient->sendBytes("GET /" + std::string(200, 'x')
                                                      + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
        static_cast<void>(longTargetClient->pumpUntil(
                [&longTargetClient]()
                {
                    return longTargetClient->receivedText().find("HTTP/1.1 4") != std::string::npos || longTargetClient->isClosedByPeer();
                },
                kWaitTimeout));
        const std::string &longTargetText = longTargetClient->receivedText();
        // 本框架把「请求目标过长」归进 HeaderTooLarge 一类：回 431，没有 414 这条映射
        samples.check(longTargetText.find("HTTP/1.1 431") != std::string::npos && longTargetText.find("HTTP/1.1 414") == std::string::npos,
                      "请求目标越界也按 431 收口（框架不产 414：它把请求行超限归进头部过大一类）");

        // h2 路径只用 maximumBodySize：越界回 413 并请对端别再传（RST_STREAM NO_ERROR），连接照旧可用
        const std::unique_ptr<Http2LoopbackClient> h2Client = Http2LoopbackClient::openOverTls(port, "h2", {});
        if (!h2Client->isHandshakeComplete() || !performHttp2Handshake(*h2Client))
        {
            samples.check(false, "限额服务器上的 h2 连接没能完成协商");
            return;
        }
        static_cast<void>(h2Client->sendBytes(makeRequestHeadersFrame(1U, makePostRequestHeaderBlock("/echo", true), false)));
        static_cast<void>(h2Client->sendBytes(
                Net::encodeHttp2DataFrame(Net::Http2DataPayload{.endStream = false, .data = std::string(64U, 'x')}, 1U)));
        static_cast<void>(h2Client->pumpUntil(
                [&h2Client]()
                {
                    return h2Client->hasResponseHeaderBlock(1U) && h2Client->resetStreamCode(1U).has_value();
                },
                kWaitTimeout));
        const std::optional<Net::Http2ErrorCode> abortCode = h2Client->resetStreamCode(1U);
        samples.check(h2Client->responseHeaderValue(1U, ":status") == "413" && abortCode.has_value()
                              && abortCode.value() == Net::Http2ErrorCode::NoError && !h2Client->sawGoAway(),
                      "h2 正文越界回 413 并用 RST_STREAM(NO_ERROR) 请对端停止上传，连接没被收掉");

        static_cast<void>(h2Client->sendBytes(makeRequestHeadersFrame(3U, makeGetRequestHeaderBlock("/hello", true), true)));
        static_cast<void>(h2Client->pumpUntil(
                [&h2Client]()
                {
                    return h2Client->isResponseCompleteOnStream(3U);
                },
                kWaitTimeout));
        samples.check(h2Client->responseHeaderValue(3U, ":status") == "200" && h2Client->responsePayload(3U) == "served-hello",
                      "越界请求之后的另一条流仍被正常服务（单流越界不牵连整条连接）");
    }

    // ============================================================================
    // 三. 明文 h2c（先验知识）与优雅关停的 GOAWAY
    // ============================================================================

    void demonstrateCleartextHttp2AndGracefulShutdown(const std::uint16_t port)
    {
        auto &samples        = Samples::checklist();
        // 声明顺序即生命周期顺序：这两个都要比 host 活得久（host 析构前，循环线程上的处理器还会碰它们）
        std::atomic<bool> handlerEntered{false};
        Net::HttpServer  *cleartextServer = nullptr;

        RunningServerHost host(port,
                               [&handlerEntered, &cleartextServer](Core::EventLoop &loop,
                                                                   const Core::InetAddress &address) -> std::unique_ptr<Net::TcpServer>
                               {
                                   auto server = std::make_unique<Net::HttpServer>(loop, address);
                                   cleartextServer = server.get();
                                   server->setLimits(makeLongTimeoutLimits());
                                   server->setIdleCheckInterval(std::chrono::milliseconds{50});
                                   // 明文端口按 h2 服务（先验知识）：对端不发前奏就被 GOAWAY 收口，不做嗅探与回退
                                   server->setHttp2CleartextEnabled(true);
                                   installSampleRoutes(server->router());
                                   installInflightRoute(server->router(), loop, handlerEntered);
                                   return server;
                               });
        if (!host.isBuilt() || !host.awaitAcceptingLoop())
        {
            samples.check(false, "h2c 服务器没能起来：" + (host.isBuilt() ? host.startFailureReason() : host.failureReason()));
            return;
        }
        samples.check(cleartextServer->isHttp2CleartextEnabled(),
                      "setHttp2CleartextEnabled(true) 落到了服务器上（读回来确实是明文 h2）");

        // —— 1. 先验知识的往返：没有 TLS、没有 Upgrade，前奏直发 ——
        const std::unique_ptr<Http2LoopbackClient> client = Http2LoopbackClient::openCleartext(port);
        samples.check(runGuarded("明文 h2c 往返",
                                 [&client]()
                                 {
                                     if (!client->isChannelReady() || !performHttp2Handshake(*client))
                                     {
                                         return false;
                                     }
                                     static_cast<void>(
                                             client->sendBytes(makeRequestHeadersFrame(1U, makeGetRequestHeaderBlock("/hello", false), true)));
                                     static_cast<void>(client->pumpUntil(
                                             [&client]()
                                             {
                                                 return client->isResponseCompleteOnStream(1U);
                                             },
                                             kWaitTimeout));
                                     return client->responseHeaderValue(1U, ":status") == "200" && client->responsePayload(1U) == "served-hello"
                                            && !client->sawGoAway();
                                 }),
                      "明文连接按先验知识说 h2：前奏 + SETTINGS 交换后 GET 得到 200 与正文，且没有 GOAWAY");

        // —— 2. 同一个端口上发 HTTP/1.1：协议唯一，前奏校验失败即收口 ——
        const std::unique_ptr<Http2LoopbackClient> wrongProtocolClient = Http2LoopbackClient::openCleartext(port);
        samples.check(runGuarded("明文端口上的 HTTP/1.1",
                                 [&wrongProtocolClient]()
                                 {
                                     if (!wrongProtocolClient->isChannelReady())
                                     {
                                         return false;
                                     }
                                     static_cast<void>(wrongProtocolClient->sendBytes("GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n"));
                                     static_cast<void>(wrongProtocolClient->pumpUntil(
                                             [&wrongProtocolClient]()
                                             {
                                                 return wrongProtocolClient->sawGoAway();
                                             },
                                             kWaitTimeout));
                                     if (wrongProtocolClient->goAwayErrorCode() != Net::Http2ErrorCode::ProtocolError)
                                     {
                                         LOG_ERROR_FMT("GOAWAY 带的错误码是 {}", Net::http2ErrorCodeName(wrongProtocolClient->goAwayErrorCode()));
                                     }
                                     return wrongProtocolClient->sawGoAway()
                                            && wrongProtocolClient->goAwayErrorCode() == Net::Http2ErrorCode::ProtocolError
                                            && wrongProtocolClient->pumpUntil(
                                                    [&wrongProtocolClient]()
                                                    {
                                                        return wrongProtocolClient->isClosedByPeer();
                                                    },
                                                    kWaitTimeout);
                                 }),
                      "h2c 端口上收到 HTTP/1.1 报文时按 PROTOCOL_ERROR 发 GOAWAY 并收口连接（不回退、不嗅探）");

        // —— 3. 框架客户端只会 h1：这个端口上它拿不到响应 ——
        samples.check(runGuarded("HttpClient 打 h2c 端口",
                                 [&port]()
                                 {
                                     const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/hello";
                                     const std::unique_ptr<Net::HttpClientResponse> response =
                                             requestWithHttpClient(url, std::chrono::milliseconds{3000});
                                     if (response != nullptr)
                                     {
                                         LOG_ERROR_FMT("HttpClient 竟然拿到了 {}：明文端不该被 h1 服务", response->statusCode);
                                     }
                                     return response == nullptr;
                                 }),
                      "Net::HttpClient 在 h2c 端口上返回空响应：出站客户端没有 HTTP/2 通道，只能靠手拼帧驱动");

        // —— 4. 优雅关停：在途请求做完，然后才发收尾 GOAWAY ——
        // 先确认处理器真的跑起来了（连接已被标成在途），再发起 drain，否则「等完在途」这句就落不到实处
        static_cast<void>(client->sendBytes(makeRequestHeadersFrame(3U, makeGetRequestHeaderBlock("/slow", false), true)));
        const bool isHandlerRunning = Samples::waitUntil(
                [&handlerEntered]()
                {
                    return handlerEntered.load(std::memory_order_acquire);
                },
                kWaitTimeout, std::chrono::milliseconds{2});
        const bool isDrained = host.requestGracefulDrain(kGracefulDrainTimeout);
        static_cast<void>(client->pumpUntil(
                [&client]()
                {
                    return client->isResponseCompleteOnStream(3U) && client->sawGoAway();
                },
                kWaitTimeout));
        samples.check(isHandlerRunning && isDrained && client->responseHeaderValue(3U, ":status") == "200"
                              && client->responsePayload(3U) == "slow-finished",
                      "优雅关停没有在途请求做完之前就收手：慢请求仍拿到完整的 200 响应");
        samples.check(client->sawGoAway() && client->goAwayErrorCode() == Net::Http2ErrorCode::NoError
                              && client->goAwayLastStreamId() == 3U,
                      "关停前发出带 NO_ERROR 的收尾 GOAWAY，last-stream-id 是已受理的最后一条流");
        LOG_INFO_FMT("收尾 GOAWAY 带的调试文本：{}", client->goAwayDebugText());

        const bool isChannelClosed = client->pumpUntil(
                [&client]()
                {
                    return client->isClosedByPeer();
                },
                kWaitTimeout);
        const Net::HttpServerStats statsAfterDrain = cleartextServer->stats();
        samples.check(isChannelClosed && !host.server().isRunning() && statsAfterDrain.activeConnectionCount == 0U,
                      "drain 之后连接已关闭、服务器不再接受新连接，活跃连接数回落为零");
    }
} // namespace

int main(const int argc, char **argv)
{
    Samples::setupConsoleLogging();
    LOG_INFO("=== Net 的 TLS/HTTP2 子系统示例开始 ===");

    const TlsMaterial material = readTlsMaterial(argc, argv);
    LOG_INFO_FMT("证书材料：{}", material.describe());
    auto &samples = Samples::checklist();
    const bool isMaterialReady = material.isComplete();
    samples.check(isMaterialReady, "仓库自签夹具（回环 IP 证书与私钥、另一张不同名的证书）都能按可执行文件位置找到");
    if (!isMaterialReady)
    {
        LOG_WARN("缺证书夹具：TLS 与 ALPN 两段本轮不跑，只跑明文 h2c 那一段");
    }

    const std::uint16_t basePort = Samples::readPortArgument(argc, argv, kSamplePortOffset);
    Samples::requirePortHeadroom(basePort, 2);   // 三台服务器依次占 basePort / +1 / +2
    LOG_INFO_FMT("三台服务器依次使用端口 {} / {} / {}", basePort, basePort + 1, basePort + 2);

    if (isMaterialReady)
    {
        demonstrateTlsAlpnHttp2(material, basePort);
        demonstrateParserLimits(material, static_cast<std::uint16_t>(basePort + 1));
    }
    demonstrateCleartextHttp2AndGracefulShutdown(static_cast<std::uint16_t>(basePort + 2));

    LOG_INFO("=== Net 的 TLS/HTTP2 子系统示例结束 ===");
    return Samples::finishSample("net_https_h2_demo");
}
