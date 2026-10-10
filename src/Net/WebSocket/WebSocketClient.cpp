#include "Net/WebSocket/WebSocketClient.h"
#include "Base/Coding/Base64.h"
#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Coroutine/DeadlineGuard.h"
#include "Core/Socket/AsyncResolver.h"
#include "Core/Socket/ConnectionRace.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/TlsContext.h"
#include "Core/Tls/TlsSocket.h"
#include "Net/Http/Client/HttpResponseParser.h"
#include "Net/Http/Client/HttpOutboundConnectionPool.h"
#include "Net/Tcp/TcpStream.h"
#include "Net/WebSocket/WebSocketPeer.h"
#include "Net/WebSocket/WebSocketUtf8.h"
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace AsynGyanis::Net
{
    namespace
    {
        /// 明文与 TLS 的默认端口：Configuration.port 留 0 时按 TLS 那一位回落
        constexpr std::uint16_t kDefaultPlainTextPort = 80;
        constexpr std::uint16_t kDefaultSecurePort    = 443;
        /// ALPN 只提的那一个协议名：本类的升级走 101 那条通路（RFC 6455 §4.1）
        constexpr std::string_view kAlpnHttp11Name = "http/1.1";
        /// Sec-WebSocket-Key 的字节数（RFC 6455 §4.1 第 7 条：16 字节随机数按标准 base64 送出）
        constexpr std::size_t kClientKeyByteLength = 16;
        /// 掩码键的字节数（RFC 6455 §5.3），逐帧新取
        constexpr std::size_t kMaskKeyByteLength = 4;
        /// 关闭原因的长度上限：控制帧整体不得过 125 字节，其中状态码占 2（RFC 6455 §5.5、§5.5.1）
        constexpr std::size_t kMaximumCloseReasonLength = kWebSocketMaximumControlPayloadLength - kWebSocketCloseCodeByteLength;
        /// 正常收口那一个码：对端给的码不能上线时按 §5.5.1 回它
        constexpr std::uint16_t kNormalClosureCode = 1000;
        /**
         * @brief 从系统的密码学随机源取 length 字节
         * @details 取不到就抛出：全零或复用的键等于放弃 §5.3 要防的那种中间设施缓存重放，
         *          「降级成不随机」比这次操作失败更糟
         * @param length 需要的字节数
         * @return std::string 按「指针 + 长度」取的随机字节，可以含 NUL
         * @throws Base::Exception 随机源不可用
         */
        std::string cryptographicRandomBytes(const std::size_t length)
        {
            std::string bytes(length, '\0');
            if (::RAND_bytes(reinterpret_cast<unsigned char *>(bytes.data()), static_cast<int>(length)) != 1)
            {
                throw Base::Exception(std::format("WebSocketClient: 系统的密码学随机源取不到 {} 字节（RAND_bytes 失败）：没有它就不能为本次握手或本帧取键，"
                                                  "请检查 OpenSSL 的随机设备是否可用",
                                                  length));
            }
            return bytes;
        }
        /// 本帧的掩码键：逐帧新取，绝不复用（§5.3 的抗重放全靠这一条）
        [[nodiscard]] WebSocketMaskKey nextMaskKey()
        {
            const std::string rawBytes = cryptographicRandomBytes(kMaskKeyByteLength);
            WebSocketMaskKey  key;
            std::copy_n(rawBytes.begin(), static_cast<std::ptrdiff_t>(kMaskKeyByteLength), key.bytes.begin());
            return key;
        }
        /// 按 TLS 那一位取实际要连的端口
        [[nodiscard]] std::uint16_t resolvePort(const WebSocketClient::Configuration &configuration)
        {
            if (configuration.port != 0)
            {
                return configuration.port;
            }
            return configuration.clientTls != nullptr ? kDefaultSecurePort : kDefaultPlainTextPort;
        }
        /**
         * @brief Host 头部要写的权威标识
         * @details 默认端口不写端口号（RFC 9110 §4.2.2：Host 取 URI 的权威部分，而默认端口在规范化时
         *          被去掉）；非默认端口必须带上，否则按名字装虚拟主机的服务端选不出该回哪份证书
         */
        [[nodiscard]] std::string hostHeaderFor(const std::string &hostName, const std::uint16_t port, const bool isTls)
        {
            const std::uint16_t defaultPort = isTls ? kDefaultSecurePort : kDefaultPlainTextPort;
            if (port == defaultPort)
            {
                return hostName;
            }
            return hostName + ":" + std::to_string(port);
        }
        /// 主机名是不是 IP 字面量：IP 与 DNS 名的证书校验规则不同，必须分开设（RFC 6125 §6.3）
        [[nodiscard]] bool isIpLiteralAddress(const std::string &hostName)
        {
            return Core::InetAddress::parseLiteral(hostName, 0).has_value();
        }
        /**
         * @brief 从 Close 帧的负载里取出对端那个状态码
         * @param payload 控制帧负载；不足 2 字节表示对端没给码（RFC 6455 §5.5.1 允许）
         * @return std::optional<std::uint16_t> 状态码原值；没给码时为空
         */
        [[nodiscard]] std::optional<std::uint16_t> readPeerCloseCode(const std::string_view payload)
        {
            if (payload.size() < kWebSocketCloseCodeByteLength)
            {
                return std::nullopt;
            }
            // 大端：先高字节，与 buildWebSocketClosePayload 的写侧逐字对应
            return static_cast<std::uint16_t>((static_cast<std::uint16_t>(static_cast<unsigned char>(payload[0])) << 8U) |
                                              static_cast<std::uint16_t>(static_cast<unsigned char>(payload[1])));
        }
        /// 一次交换用的读块大小：与 HttpClient 同档，够让 101 的头块在少数几轮里读完
        constexpr std::size_t kExchangeChunkByteLength = 4096;
        /**
         * @brief 升级应答的读取进度
         * @details 五个出口各对应一种线上事实：缺一个出口就意味着某种「对端不说话」的形状没人管
         */
        enum class UpgradeReadOutcome
        {
            NeedsMoreBytes,    ///< 头块还没收齐，继续读
            ResponseComplete,  ///< 一条完整应答到手
            MalformedResponse, ///< 报文不合规
            PeerClosed,        ///< 对端在给出完整应答之前收口
            ReadFailed         ///< 读错误（含本端到时限把套接字关掉那一种）
        };
        /// 升级交换的产出：解析好的应答，以及头块之后一起到的那截帧字节
        struct UpgradeExchange
        {
            HttpResponseInfo response;  ///< 解析完成的应答（状态码与头部）
            std::string      headBytes; ///< 请求原文，写失败时不参与结论
            std::string      leftover;  ///< 头块结束之后的字节：属于第一条帧，一个都不能丢
        };
        /**
         * @brief 把刚读到的一段字节喂给解析器，并给出这一轮的进度
         * @details 两条通路（明文与 TLS）共用这一份判定：进度口径与拒因只在「读」这一步分岔
         * @param parser 本次交换专用的解析器
         * @param leftover 出参：头块之后到的字节累加在这里
         * @param data 刚读到的缓冲区首地址
         * @param length 刚读到的字节数
         * @param isEndOfStream 对端是否已正常收口（读到 0 字节）
         * @return UpgradeReadOutcome 这一轮的结论
         */
        UpgradeReadOutcome feedUpgradeBytes(HttpResponseParser &parser, std::string &leftover, const char *const data, const std::size_t length, const bool isEndOfStream)
        {
            if (isEndOfStream)
            {
                return UpgradeReadOutcome::PeerClosed;
            }
            const std::size_t consumedByte = parser.feed(std::string_view(data, length));
            if (parser.hasFailed())
            {
                return UpgradeReadOutcome::MalformedResponse;
            }
            if (!parser.isComplete())
            {
                return UpgradeReadOutcome::NeedsMoreBytes;
            }
            // 101 之后紧跟着的就是帧字节：解析器只消化到头块结束，剩下的必须留给解码器，否则服务端
            // 「升级应答与第一条消息一起发」这种写法会被截掉半条消息
            if (consumedByte < length)
            {
                leftover.append(data + consumedByte, length - consumedByte);
            }
            return UpgradeReadOutcome::ResponseComplete;
        }
        /**
         * @brief 把一条应答读到底：明文通路
         * @details 刻意不走 HttpOutboundConnection：那条通路的 parser 是按「带正文的响应」设计的，
         *          而升级交换之后这条连接就不再是 HTTP 了。这一段本轮不设时限（理由见类注释的 @warning）
         * @param stream 已连上的明文流
         * @param requestText 升级请求原文
         * @param target 目标描述，只进拒因文本
         * @return Core::Task<std::expected<UpgradeExchange, std::string>> 一条完整应答与它之后的那截字节
         */
        Core::Task<std::expected<UpgradeExchange, std::string>> exchangeOverPlain(TcpStream &stream, const std::string_view requestText, const std::string target)
        {
            std::array<char, kExchangeChunkByteLength> chunk{};
            UpgradeExchange                            exchange;
            HttpResponseParser                         parser;
            try
            {
                co_await stream.writeAll(requestText.data(), requestText.size());
            } catch (const Base::Exception &failure)
            {
                co_return std::unexpected(std::format("升级请求没能写进通路（目标 {}）：{}。底层原因：{}", target, "连接在握手期间断开或已到时限", failure.what()));
            }
            while (true)
            {
                const auto received = co_await stream.read(chunk.data(), chunk.size());
                if (received < 0)
                {
                    co_return std::unexpected(std::format("读取 101 应答时通路出错（目标 {}）：被对端复位或本端被撤", target));
                }
                const auto outcome = feedUpgradeBytes(parser, exchange.leftover, chunk.data(), received == 0 ? 0 : static_cast<std::size_t>(received), received == 0);
                if (outcome == UpgradeReadOutcome::NeedsMoreBytes)
                {
                    continue;
                }
                if (outcome == UpgradeReadOutcome::PeerClosed)
                {
                    co_return std::unexpected(std::format("对端在给出 101 应答之前就把连接关了（目标 {}）：它可能没把这个路径当作 WebSocket 端点", target));
                }
                if (outcome == UpgradeReadOutcome::MalformedResponse)
                {
                    co_return std::unexpected(std::format("对端的升级应答不是合法 HTTP 报文（目标 {}）：状态行或头部读不懂，或越出了声明的上限", target));
                }
                exchange.response = parser.result();
                co_return exchange;
            }
        }
        /**
         * @brief 把一条应答读到底：TLS 通路
         * @details 与明文那一条同一条判定，只是读写走 TlsSocket；这一段同样不设时限（见类注释 @warning）
         * @param tlsSocket 已完成握手的客户端 TLS 套接字
         * @param requestText 升级请求原文
         * @param target 目标描述，只进拒因文本
         * @return Core::Task<std::expected<UpgradeExchange, std::string>> 一条完整应答与它之后的那截字节
         */
        Core::Task<std::expected<UpgradeExchange, std::string>> exchangeOverTls(Core::TlsSocket &tlsSocket, const std::string_view requestText, const std::string target)
        {
            std::array<char, kExchangeChunkByteLength> chunk{};
            UpgradeExchange                            exchange;
            HttpResponseParser                         parser;
            try
            {
                const ssize_t writtenByte = co_await tlsSocket.asyncSend(requestText.data(), requestText.size());
                if (writtenByte <= 0)
                {
                    co_return std::unexpected(std::format("升级请求没能写进通路（目标 {}）：通路在握手期间被关掉或已到时限", target));
                }
            } catch (const Base::Exception &failure)
            {
                co_return std::unexpected(std::format("升级请求没能写进通路（目标 {}）：{}。底层原因：{}", target, "TLS 写出失败", failure.what()));
            }
            while (true)
            {
                ssize_t received = 0;
                try
                {
                    received = co_await tlsSocket.asyncReceive(chunk.data(), chunk.size());
                } catch (const Base::Exception &failure)
                {
                    co_return std::unexpected(std::format("读取 101 应答时通路出错（目标 {}）：通路被关掉——被对端复位，或本端把会话撤了。底层原因：{}", target, failure.what()));
                }
                const auto outcome = feedUpgradeBytes(parser, exchange.leftover, chunk.data(), received == 0 ? 0 : static_cast<std::size_t>(received), received == 0);
                if (outcome == UpgradeReadOutcome::NeedsMoreBytes)
                {
                    continue;
                }
                if (outcome == UpgradeReadOutcome::PeerClosed)
                {
                    co_return std::unexpected(std::format("对端在给出 101 应答之前就把连接关了（目标 {}）：它可能没把这个路径当作 WebSocket 端点", target));
                }
                if (outcome == UpgradeReadOutcome::MalformedResponse)
                {
                    co_return std::unexpected(std::format("对端的升级应答不是合法 HTTP 报文（目标 {}）：状态行或头部读不懂，或越出了声明的上限", target));
                }
                exchange.response = parser.result();
                co_return exchange;
            }
        }
        /**
         * @brief 在已经连上的套接字上完成客户端 TLS 握手
         * @details 三件事缺一不可：SNI（让按名字装虚拟主机的服务端挑对证书）、证书主机名校验
         *          （只验链不验名字，任何受信 CA 给他域签的证书都能冒充目标，CWE-297）、ALPN 只提
         *          http/1.1（协商出 h2 就没有 101 而是扩展 CONNECT，帧的形状与本类不是一套）。
         * @param loop 所属事件循环
         * @param context 调用方持有的客户端 TLS 上下文
         * @param socket 已经连上的套接字，所有权交给本函数
         * @param hostName 用于 SNI 与主机名校验的名字
         * @param handshakeBudget TLS 那一段的剩余时限；空表示已经没有预算可用
         * @return Core::Task<std::expected<std::unique_ptr<Core::TlsSocket>, std::string>> 成功交出套接字，失败给中文拒因
         */
        Core::Task<std::expected<std::unique_ptr<Core::TlsSocket>, std::string>> makeClientTlsSocket(Core::EventLoop &loop, const Core::TlsContext &context,
                                                                                                     Core::AsyncSocket socket, const std::string hostName,
                                                                                                     const std::optional<std::chrono::milliseconds> handshakeBudget)
        {
            if (!handshakeBudget.has_value())
            {
                co_return std::unexpected(std::format("WebSocket 出站的握手时限已用尽：还没开始 TLS 握手（主机 {}）", hostName));
            }
            // createSSL 把 SSL_new 与 SSL_set_fd 一起做了：少了绑定这一步 SSL_connect 立刻失败，
            // 而错误队列是空的，现场只剩「握手失败」四个字
            SSL *ssl = context.createSSL(socket.fileDescriptor());
            SSL_set_tlsext_host_name(ssl, hostName.c_str());
            if (isIpLiteralAddress(hostName))
            {
                X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), hostName.c_str());
            } else
            {
                SSL_set1_host(ssl, hostName.c_str());
            }
            // ALPN 的线格式是「单字节长度 + 协议名」的串联：填错不会报错，只会对端一条都匹配不上
            std::string encodedProtocolNames;
            encodedProtocolNames.push_back(static_cast<char>(kAlpnHttp11Name.size()));
            encodedProtocolNames.append(kAlpnHttp11Name);
            if (::SSL_set_alpn_protos(ssl, reinterpret_cast<const unsigned char *>(encodedProtocolNames.data()), static_cast<unsigned int>(encodedProtocolNames.size())) != 0)
            {
                // 还没交给 TlsSocket，这份 SSL 归本函数收尾：漏掉 SSL_free 就是一次带引用的泄漏
                SSL_free(ssl);
                co_return std::unexpected("TLS 没能设置 ALPN 协议列表：OpenSSL 拒绝了这份写法");
            }
            auto tlsSocket = std::make_unique<Core::TlsSocket>(ssl, loop, std::move(socket), Core::TlsSocket::Role::Client);
            // 看门狗到点即关套接字：否则「对端不谈了」会让这条协程永久挂着，这一段没有别的出口
            const Core::DeadlineGuard<Core::TlsSocket> tlsDeadline(loop, *tlsSocket, *handshakeBudget, "WebSocketClient");
            try
            {
                co_await tlsSocket->handshake();
            } catch (const Base::Exception &failure)
            {
                co_return std::unexpected(
                        std::format("TLS 握手失败：证书没通过校验、协议或密码套件不匹配，或对端在握手中途收线（主机 {}）。底层原因：{}", hostName, failure.what()));
            }
            co_return tlsSocket;
        }
        /**
         * @brief 把一条 101 应答核到底，并核对它选定的子协议
         * @details accept 那一格由 `validateWebSocketUpgradeResponse` 判；这里补上只有客户端才判得了
         *          的一条：对端选定的名字必须在本端提议过的名单里（RFC 6455 §4.1 对客户端的要求）——
         *          不在名单里意味着两边对「这条连接说什么协议」并没有共识，留着比拒掉更糟
         * @param response 交换到的应答（状态码与头部）
         * @param clientKey 本端这次握手发出去的 key
         * @param subprotocols 本端提议过的子协议名单
         * @return std::expected<WebSocketUpgradeAgreement, std::string> 谈成的事实，或中文拒因
         */
        [[nodiscard]] std::expected<WebSocketUpgradeAgreement, std::string> checkUpgradeAgreement(const HttpResponseInfo &response, const std::string_view clientKey,
                                                                                                  const std::vector<std::string> &subprotocols)
        {
            auto agreement = validateWebSocketUpgradeResponse(response.statusCode, response.headers, clientKey);
            if (!agreement.has_value())
            {
                return std::unexpected(agreement.error());
            }
            if (!agreement->acceptedSubprotocol.empty() && std::find(subprotocols.begin(), subprotocols.end(), agreement->acceptedSubprotocol) == subprotocols.end())
            {
                return std::unexpected(std::format("对端选定的子协议「{}」不在本端提议的名单里（RFC 6455 §4.1）：请核对对面的实现，或把这个名字加进 Configuration.subprotocols",
                                                   agreement->acceptedSubprotocol));
            }
            return *agreement;
        }
    } // namespace
    WebSocketClient::WebSocketClient(std::unique_ptr<HttpOutboundConnection> transport, WebSocketUpgradeAgreement agreement, const Configuration &configuration,
                                     std::string pendingBytes) :
        m_transport(std::move(transport)), m_agreement(std::move(agreement)), m_readBuffer(kReadChunkByteLength, '\0'), m_inboundBytes(std::move(pendingBytes))
    {
        // 本端是客户端：对端（服务端）发来的帧必须不带掩码，带了就当场判错（RFC 6455 §5.1）
        m_decoder.setMaskingRequirement(WebSocketMaskingRequirement::PeerMustNotMask);
        m_decoder.setMaximumMessagePayloadLength(configuration.maximumMessageSize);
    }
    WebSocketClient::~WebSocketClient() = default;
    Core::Task<std::expected<std::unique_ptr<WebSocketClient>, std::string>> WebSocketClient::connect(Core::EventLoop &loop, Configuration configuration)
    {
        // 用法错误在任何资源获取之前判完：解析与连接都是要归还的东西，带着坏参数进去就会留下一条
        // 半途的连接，或者一条永远没人接的协程帧
        if (configuration.hostName.empty())
        {
            co_return std::unexpected("WebSocket 出站缺 Configuration.hostName：请给出目标主机名或 IP 字面量（它同时用作 Host、SNI 与证书主机名校验的对象）");
        }
        if (configuration.handshakeTimeout <= std::chrono::milliseconds::zero())
        {
            co_return std::unexpected(std::format("WebSocket 出站的 handshakeTimeout 是 {} 毫秒：建连与 TLS 握手这两段必须各有时限，0 或负数会让协程停在那两段里出不来。"
                                                  "要放宽就写一个更大的正数；等 101 那一段本轮不设时限（类注释的 @warning 写明）",
                                                  configuration.handshakeTimeout.count()));
        }
        const auto startedAt = std::chrono::steady_clock::now();
        // 三段共用一份总时限，各拿「此刻还剩下多少」：连接、TLS 握手、等 101 缺一段都会让总时限形同虚设
        const auto remainingBudget = [&]() -> std::optional<std::chrono::milliseconds>
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);
            const auto left    = configuration.handshakeTimeout - elapsed;
            if (left <= std::chrono::milliseconds::zero())
            {
                return std::nullopt;
            }
            return left;
        };
        const std::uint16_t port  = resolvePort(configuration);
        const bool          isTls = configuration.clientTls != nullptr;
        const std::string   target{configuration.hostName + ":" + std::to_string(port)};
        const auto          resolved = co_await Core::AsyncResolver::resolve(loop, configuration.hostName, port);
        if (resolved.empty())
        {
            co_return std::unexpected(std::format("WebSocket 出站没能把「{}」解析成可用地址：请核对主机名，或直接给 Configuration.hostName 填 IP 字面量", configuration.hostName));
        }
        const auto connectBudget = remainingBudget();
        if (!connectBudget.has_value())
        {
            co_return std::unexpected(std::format("WebSocket 出站的 {} 毫秒握手时限已用尽：还没开始建立 TCP 连接（目标 {}）", configuration.handshakeTimeout.count(), target));
        }
        auto candidate = co_await Core::connectCandidates(loop, Core::orderForConnectionRace(resolved), *connectBudget);
        if (!candidate.has_value())
        {
            co_return std::unexpected(std::format("建立 TCP 连接失败：对端拒绝、不可达或还没连上就超时（目标 {}）", target));
        }
        // 时限从「连上了」这一刻起算：握手那一段（写出请求 + 等到 101）拿剩余的预算
        // 这把 key 只用于本次握手：accept 由它算出，核对 101 全靠这条对应关系
        const std::string clientKey = Base::base64Encode(cryptographicRandomBytes(kClientKeyByteLength));
        auto requestText = buildWebSocketUpgradeRequest(hostHeaderFor(configuration.hostName, port, isTls), configuration.requestTarget, clientKey, configuration.subprotocols);
        if (!requestText.has_value())
        {
            // 用法错误（Host、请求目标或子协议名里带了控制字符之类）：拒因原样交给调用方，
            // 它点得名是哪一段取值不合用
            co_return std::unexpected(requestText.error());
        }
        // 升级交换走本端自己持有的套接字对象，做完才把通路交给 HttpOutboundConnection 承载帧
        std::optional<TcpStream>                                   plainStream;
        std::unique_ptr<Core::TlsSocket>                           tlsSocket;
        std::optional<std::expected<UpgradeExchange, std::string>> exchanged;
        if (isTls)
        {
            auto secured = co_await makeClientTlsSocket(loop, *configuration.clientTls, std::move(candidate->socket), configuration.hostName, remainingBudget());
            if (!secured.has_value())
            {
                co_return std::unexpected(secured.error());
            }
            tlsSocket = std::move(*secured);
            exchanged = co_await exchangeOverTls(*tlsSocket, *requestText, target);
        } else
        {
            plainStream.emplace(std::move(candidate->socket));
            exchanged = co_await exchangeOverPlain(*plainStream, *requestText, target);
        }
        if (!exchanged->has_value())
        {
            co_return std::unexpected(exchanged->error());
        }
        HttpOutboundEndpointKey                 endpointKey{configuration.hostName, port, isTls};
        std::unique_ptr<HttpOutboundConnection> transport;
        if (isTls)
        {
            transport = HttpOutboundConnection::forSecure(endpointKey, std::move(tlsSocket));
        } else
        {
            // TcpStream 内部的读缓冲留着没消费的字节：换到通路之后它们仍在同一条流上，不会丢
            transport = HttpOutboundConnection::forPlain(endpointKey, std::move(*plainStream));
        }
        auto agreement = checkUpgradeAgreement((*exchanged)->response, clientKey, configuration.subprotocols);
        if (!agreement.has_value())
        {
            co_return std::unexpected(agreement.error());
        }
        co_return std::unique_ptr<WebSocketClient>(new WebSocketClient(std::move(transport), std::move(*agreement), configuration, std::move((*exchanged)->leftover)));
    }
    const WebSocketUpgradeAgreement &WebSocketClient::agreement() const noexcept
    {
        return m_agreement;
    }
    bool WebSocketClient::isOpen() const noexcept
    {
        return m_isOpen;
    }
    Core::Task<std::expected<WebSocketFrame, std::string>> WebSocketClient::receive()
    {
        // 收侧只看通路活着没有：本端发过 Close 之后仍然要能读回对端那条 Close（§5.5.1 的握手是双向的），
        // 「还能不能再发」由 m_isOpen 单独管
        if (!m_transport->isOpen())
        {
            co_return std::unexpected("通路已经关了：这条会话收不到后续消息");
        }
        for (;;)
        {
            const WebSocketDecodeStatus status = m_decoder.parse(m_inboundBytes.data(), m_inboundBytes.size());
            // 解码器一次只消化到帧尾为止：帧之后的字节留在 m_inboundBytes 里，下一轮接着喂
            m_inboundBytes.erase(0, m_decoder.consumedByteCount());
            if (status == WebSocketDecodeStatus::Error)
            {
                // 上限越出按 1009、其余按 1002 尽力告知对端（RFC 6455 §7.4.1）；通路已死时这一步作罢，
                // 原因已经在交回的字符串里
                const std::uint16_t closeCode = m_decoder.isLimitExceeded() ? kWebSocketMessageTooBigCode : kWebSocketProtocolErrorCode;
                const std::string   errorText = m_decoder.errorMessage();
                // 先把「为什么收口」发出去，再把本端置成不可用：反过来写会让这条出声的帧被自家的闸门拦下，
                // 对端只看到一个没有原因的断开
                static_cast<void>(co_await writeFrameBytes(prepareFrame(WebSocketOpCode::Close, buildWebSocketClosePayload(closeCode, {}))));
                m_isOpen = false;
                m_transport->close();
                co_return std::unexpected(errorText);
            }
            if (status == WebSocketDecodeStatus::Frame)
            {
                const WebSocketFrame frame = m_decoder.takeFrame();
                if (frame.opCode == WebSocketOpCode::Ping)
                {
                    // 心跳必须按原负载回 Pong（RFC 6455 §5.5.3）：不回就是让对端把这条连接判成死的
                    if (!co_await writeFrameBytes(prepareFrame(WebSocketOpCode::Pong, frame.payload)))
                    {
                        m_isOpen = false;
                        co_return std::unexpected("回应 Pong 时通路已不可用：这次心跳没能答出去，连接按已断开处理");
                    }
                    continue;
                }
                if (frame.opCode == WebSocketOpCode::Pong)
                {
                    // Pong 在这一格消费掉：调用方要的是数据消息。发过 Ping 之后「有没有别的流量」由上层
                    // 自己计时，本层不替它攒状态
                    continue;
                }
                if (frame.opCode == WebSocketOpCode::Close)
                {
                    co_return co_await answerPeerClose(frame);
                }
                co_return frame;
            }
            const auto received = co_await m_transport->receive(m_readBuffer.data(), m_readBuffer.size());
            if (received == 0)
            {
                m_isOpen = false;
                co_return std::unexpected("对端没有留下 Close 帧就收了线（RFC 6455 §7.1.1 的快速断开）：本端按已关闭处理");
            }
            if (received < 0)
            {
                m_isOpen = false;
                co_return std::unexpected("读取帧时通路出错：连接已断开、被对端复位，或本端把它关了");
            }
            m_inboundBytes.append(m_readBuffer.data(), static_cast<std::size_t>(received));
        }
    }
    Core::Task<std::expected<WebSocketFrame, std::string>> WebSocketClient::answerPeerClose(const WebSocketFrame &closeFrame)
    {
        // 回一条 Close 并把对端那个码原样答回去（RFC 6455 §5.5.1）；码不能上线时改回 1000——
        // 把保留值发出去只会让对面把这次收口读成协议错误
        std::uint16_t echoCode = kNormalClosureCode;
        if (const std::optional<std::uint16_t> peerCode = readPeerCloseCode(closeFrame.payload); peerCode.has_value() && isAllowedWebSocketCloseCode(*peerCode))
        {
            echoCode = *peerCode;
        }
        // 原因不回送：§5.5.1 只要求把状态码答回去，而对面那条原因是给对面自己看的
        co_await writeFrameBytes(prepareFrame(WebSocketOpCode::Close, buildWebSocketClosePayload(echoCode, {})));
        m_isOpen = false;
        m_transport->close();
        co_return closeFrame;
    }
    Core::Task<bool> WebSocketClient::sendText(const std::string_view text)
    {
        return writeFrameBytes(prepareFrame(WebSocketOpCode::Text, text));
    }
    Core::Task<bool> WebSocketClient::sendBinary(const std::string_view binary)
    {
        return writeFrameBytes(prepareFrame(WebSocketOpCode::Binary, binary));
    }
    Core::Task<bool> WebSocketClient::sendPing(const std::string_view payload)
    {
        return writeFrameBytes(prepareFrame(WebSocketOpCode::Ping, payload));
    }
    Core::Task<bool> WebSocketClient::close(const std::uint16_t statusCode, const std::string_view reason)
    {
        // 三道用法判据与收侧同解，而且都在这一步跑完：自己不收的东西绝不发出去，否则一次正常收口会在
        // 对端变成协议错误，而业务想看的关闭原因也就丢了。抛出发生在调用点，不建协程帧也不留半条帧在线上
        if (!isAllowedWebSocketCloseCode(statusCode))
        {
            throw Base::InvalidArgumentException(std::format("WebSocketClient::close：状态码 {} 不允许出现在线上（RFC 6455 §7.4.1/§7.4.2：1004/1005/1006/1015 为保留值，"
                                                             "1016-2999 段未经注册）：请改用 1000-1003、1007-1014 或 3000-4999 段的值",
                                                             statusCode));
        }
        if (reason.size() > kMaximumCloseReasonLength)
        {
            throw Base::InvalidArgumentException(std::format("WebSocketClient::close：关闭原因 {} 字节超过上限 {} 字节（控制帧整体不得超过 {} 字节，其中状态码占 {} 字节，"
                                                             "RFC 6455 §5.5）：请缩短原因，或把长说明改用 sendText() 作为消息发出",
                                                             reason.size(), kMaximumCloseReasonLength, kWebSocketMaximumControlPayloadLength, kWebSocketCloseCodeByteLength));
        }
        if (const std::size_t invalidByteOffset = findInvalidWebSocketUtf8ByteOffset(reason); invalidByteOffset != std::string_view::npos)
        {
            throw Base::InvalidArgumentException(std::format("WebSocketClient::close：关闭原因从第 {} 个字节起不是合法 UTF-8（RFC 6455 §7.4 要求状态码之后的正文按 UTF-8 "
                                                             "编码）：这样的帧发出去对端只能按 1007 收口，请把原因改成合法文本或留空",
                                                             invalidByteOffset));
        }
        return sendCloseBytes(prepareFrame(WebSocketOpCode::Close, buildWebSocketClosePayload(statusCode, reason)));
    }
    std::string WebSocketClient::prepareFrame(const WebSocketOpCode opCode, const std::string_view payload)
    {
        // 本帧现取一把新键：编码里对控制帧长度与操作码的用法判据也在这一步抛出，调用点当场就能看见
        return encodeWebSocketFrame(opCode, payload, true, false, nextMaskKey());
    }
    Core::Task<bool> WebSocketClient::sendCloseBytes(std::string frameBytes)
    {
        // 通路已死或本端已经发过 Close：不再补第二条（§5.5.1 只要求一次关闭握手）
        if (!m_isOpen)
        {
            co_return false;
        }
        // 状态先落定再发帧：写出期间协程会挂起，此时业务若再调 send*() 必须已经看到「不能再发」。
        // 收侧不受这个标记影响——调用方还要靠 receive() 读回对端那条 Close 和它给的原因
        m_isOpen = false;
        co_return co_await m_transport->send(frameBytes);
    }
    Core::Task<bool> WebSocketClient::writeFrameBytes(std::string frameBytes)
    {
        if (!m_isOpen)
        {
            co_return false;
        }
        co_return co_await m_transport->send(frameBytes);
    }
} // namespace AsynGyanis::Net
