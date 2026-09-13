// TestHttp2Session.cpp —— HTTP/2 会话的端到端测试（真实 TLS 回环 + 真实 h2 帧）
//
// 覆盖五块：
//   1) 端到端服务：客户端（OpenSSL，ALPN 提 h2）握手 → 交换 SETTINGS → 发前奏 → GET → 200 与正文，
//      第二条请求验证连接复用与 request-id 回显；
//   2) 接收方向流控：超过初始窗口的 POST 必须靠服务端 WINDOW_UPDATE 才能发完（连接级与流级都要）；
//   3) 响应分片：大于对端 MAX_FRAME_SIZE 的正文按帧上限拆成多片 DATA，末片带 END_STREAM；
//   4) ALPN 分流：只提 http/1.1 的客户端仍走 HTTP/1.1 会话（普通 GET 得 200）；
//   5) 拒绝面：错误前奏回 GOAWAY 并收口、未收录方法不落进业务路由（405）、正文超限回 413、
//      WebSocket 升级回 501。
// 夹具在本文件内自建（TestHttp2Server + TlsHttp2LoopbackClient），端口由内核分配，用例之间不共用端口。
// 客户端的帧解码复用生产解码器（Http2FrameDecoder），响应的头块复用生产解码器（HpackDecoder）解回，
// 因此「服务端吐出的字节」始终由第二份实现对照。

#include "Net/Http/HttpsServer.h"
#include "Net/Http2/Http2Connection.h"
#include "Net/Http2/Http2Session.h"

#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/HttpServerStats.h"
#include "Net/Http/Router.h"
#include "Net/WebSocket/WebSocketPeer.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/Platform.h"

#include "HttpTestSupport.h"

#include <gtest/gtest.h>

#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;

        /// 仓库内预生成的自签测试证书（CN=asyngyanis-test，有效期至 2036）
        const std::filesystem::path kTestCertificatePath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_cert.pem";

        /// 仓库内预生成的配套私钥
        const std::filesystem::path kTestKeyPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_key.pem";

        /// 流控用例的 POST 正文长度：明显超过连接级与流级的初始窗口（65535），必须靠 WINDOW_UPDATE 才能发完
        constexpr std::size_t kPostBodyByteCount = 100000;

        /// 大正文响应用例的正文长度：超过对端 MAX_FRAME_SIZE（16384），应当拆成 3 片 DATA
        constexpr std::size_t kLargeResponseByteCount = 40000;

        /// 生成的 request-id 形态：4 位十六进制前缀 + '-' + 16 位十六进制序号（见 HttpRequestId.h）
        constexpr std::size_t kGeneratedRequestIdLength = 4 + 1 + 16;

        /// 生成形态里分隔前缀与序号的位置
        constexpr std::size_t kGeneratedRequestIdSeparatorIndex = 4;

        /**
         * @brief 观测性/超时用例共用的限额：超时三项都设得很长
         * @return HttpServerLimits 关掉超时保护的配置
         */
        HttpServerLimits makeLongTimeoutLimits()
        {
            HttpServerLimits limits;
            limits.idleTimeout  = std::chrono::seconds{10};
            limits.readTimeout  = std::chrono::seconds{10};
            limits.writeTimeout = std::chrono::seconds{10};
            return limits;
        }

        /**
         * @brief 判断一段文本是否是服务器生成的 request-id
         * @param requestId 待判定的标识
         * @return true 长度符合约定，且分隔符位置与其余字符都是小写十六进制
         */
        bool looksLikeGeneratedRequestId(const std::string_view requestId)
        {
            if (requestId.size() != kGeneratedRequestIdLength || requestId[kGeneratedRequestIdSeparatorIndex] != '-')
            {
                return false;
            }

            for (std::size_t index = 0; index < requestId.size(); ++index)
            {
                if (index == kGeneratedRequestIdSeparatorIndex)
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
         * @brief 测试用的一帧：类型、标志、流号与净负载
         */
        struct TestFrame
        {
            Http2FrameType type{Http2FrameType::Data}; ///< 帧类型
            std::uint8_t flags{0};                     ///< 标志位
            std::uint32_t streamId{0};                 ///< 流号
            std::string payload;                       ///< 净负载（测试不构造 padding，因此与线上负载同长）
        };

        /**
         * @brief 把生产解码器交出的一帧转成测试记录
         * @param frame 帧层交出的帧（按移动收下）
         * @return TestFrame 测试记录
         */
        TestFrame toTestFrame(Http2Frame frame)
        {
            TestFrame testFrame;
            testFrame.type = frame.header.type;
            testFrame.flags = frame.header.flags;
            testFrame.streamId = frame.header.streamId;
            testFrame.payload = std::move(frame.payload);
            return testFrame;
        }

        /**
         * @brief 统计一批帧里某种类型的条数
         * @param frames 帧列表
         * @param frameType 目标类型
         * @return std::size_t 条数
         */
        std::size_t countFrames(const std::vector<TestFrame> &frames, const Http2FrameType frameType)
        {
            return static_cast<std::size_t>(std::count_if(frames.begin(), frames.end(),
                                                          [frameType](const TestFrame &frame)
                                                          {
                                                              return frame.type == frameType;
                                                          }));
        }

        /**
         * @brief 取一批帧里第一条指定类型的帧
         * @param frames 帧列表
         * @param frameType 目标类型
         * @return const TestFrame* 命中的帧；没有时返回 nullptr
         */
        const TestFrame *findFrame(const std::vector<TestFrame> &frames, const Http2FrameType frameType)
        {
            for (const TestFrame &frame: frames)
            {
                if (frame.type == frameType)
                {
                    return &frame;
                }
            }
            return nullptr;
        }

        /**
         * @brief 取服务端第一条 SETTINGS 帧的 SETTINGS_INITIAL_WINDOW_SIZE
         * @param settingsFrame 服务端的 SETTINGS 帧
         * @param defaultWindowSize 没有该参数时的取值（规范的初值 65535）
         * @return std::uint32_t 对端通告的初始窗口
         */
        std::uint32_t readSettingsInitialWindowSize(const TestFrame &settingsFrame, const std::uint32_t defaultWindowSize)
        {
            // SETTINGS 负载是 6 字节一项的序列：2 字节标识 + 4 字节取值
            for (std::size_t offset = 0; offset + 6 <= settingsFrame.payload.size(); offset += 6)
            {
                const std::uint16_t identifier = static_cast<std::uint16_t>((static_cast<unsigned char>(settingsFrame.payload[offset]) << 8) |
                                                                            static_cast<unsigned char>(settingsFrame.payload[offset + 1]));
                std::uint32_t value = 0;
                for (std::size_t byteIndex = 0; byteIndex < 4; ++byteIndex)
                {
                    value = (value << 8) | static_cast<unsigned char>(settingsFrame.payload[offset + 2 + byteIndex]);
                }
                if (identifier == static_cast<std::uint16_t>(Http2SettingIdentifier::InitialWindowSize))
                {
                    return value;
                }
            }
            return defaultWindowSize;
        }

        /**
         * @brief 取 WINDOW_UPDATE 帧的窗口增量
         * @param payload 帧负载（4 字节大端）
         * @return std::uint32_t 增量；负载不足 4 字节时返回 0
         */
        std::uint32_t readWindowUpdateIncrement(const std::string_view payload)
        {
            if (payload.size() < 4)
            {
                return 0;
            }
            std::uint32_t increment = 0;
            for (std::size_t byteIndex = 0; byteIndex < 4; ++byteIndex)
            {
                increment = (increment << 8) | static_cast<unsigned char>(payload[byteIndex]);
            }
            return increment & 0x7FFFFFFFU;
        }

        /**
         * @brief 取 GOAWAY 帧里的错误码
         * @param payload GOAWAY 负载（前 4 字节是 last-stream-id）
         * @return Http2ErrorCode 错误码；负载不足 8 字节时返回 NoError
         */
        Http2ErrorCode readGoAwayErrorCode(const std::string_view payload)
        {
            if (payload.size() < 8)
            {
                return Http2ErrorCode::NoError;
            }
            std::uint32_t errorCode = 0;
            for (std::size_t byteIndex = 4; byteIndex < 8; ++byteIndex)
            {
                errorCode = (errorCode << 8) | static_cast<unsigned char>(payload[byteIndex]);
            }
            return static_cast<Http2ErrorCode>(errorCode);
        }

        /**
         * @brief 拼一个「索引表示」（RFC 7541 §6.1）
         * @param index 索引空间的索引（1..61 是静态表）
         * @return std::string 编码结果
         */
        std::string hpackIndexedField(const std::size_t index)
        {
            return encodeHpackInteger(index, 7, 0x80);
        }

        /**
         * @brief 拼一个「带增量索引的字面量」，名字走静态表索引（RFC 7541 §6.2.1）
         * @param staticNameIndex 静态表里的名字索引
         * @param value 头值
         * @return std::string 编码结果
         */
        std::string hpackLiteralField(const std::size_t staticNameIndex, const std::string_view value)
        {
            std::string bytes = encodeHpackInteger(staticNameIndex, 6, 0x40);
            appendHpackString(bytes, value);
            return bytes;
        }

        /**
         * @brief 拼一个「带增量索引的字面量」，名字与值都是字面量（RFC 7541 §6.2.1 的名字索引 0）
         * @param name 头名
         * @param value 头值
         * @return std::string 编码结果
         */
        std::string hpackLiteralField(const std::string_view name, const std::string_view value)
        {
            std::string bytes = encodeHpackInteger(0, 6, 0x40);
            appendHpackString(bytes, name);
            appendHpackString(bytes, value);
            return bytes;
        }

        /**
         * @brief 拼一个 GET 请求头块
         * @param path 请求路径（":path" 的值；"/" 直接用静态表项）
         * @param extraHeaderFields 追加的普通头（名 + 值），按给定顺序写在对端序列之后
         * @return std::string 头块字节
         * @details 索引取自 RFC 7541 Appendix A：2 是 :method: GET、7 是 :scheme: https、4 是 :path: /、
         *          1 是只带名字的 :authority（值只能由字面量给出）。
         */
        std::string makeGetRequestHeaderBlock(const std::string_view path,
                                              const std::vector<std::pair<std::string, std::string>> &extraHeaderFields = {})
        {
            std::string headerBlock;
            headerBlock += hpackIndexedField(2);
            headerBlock += hpackIndexedField(7);
            headerBlock += path == "/" ? hpackIndexedField(4) : hpackLiteralField(4, path);
            headerBlock += hpackLiteralField(1, "localhost");
            for (const std::pair<std::string, std::string> &extraHeaderField: extraHeaderFields)
            {
                headerBlock += hpackLiteralField(extraHeaderField.first, extraHeaderField.second);
            }
            return headerBlock;
        }

        /**
         * @brief 拼一个 POST 请求头块（不带 END_STREAM：正文随后由 DATA 帧发出）
         * @param path 请求路径
         * @return std::string 头块字节
         */
        std::string makePostRequestHeaderBlock(const std::string_view path)
        {
            std::string headerBlock;
            headerBlock += hpackLiteralField(2, "POST");
            headerBlock += hpackIndexedField(7);
            headerBlock += hpackLiteralField(4, path);
            headerBlock += hpackLiteralField(1, "localhost");
            return headerBlock;
        }

        /**
         * @brief 拼一个指定方法的请求头块（用于未收录的方法）
         * @param method 方法原文
         * @param path 请求路径
         * @return std::string 头块字节
         */
        std::string makeMethodRequestHeaderBlock(const std::string_view method, const std::string_view path)
        {
            std::string headerBlock;
            headerBlock += hpackLiteralField(2, method);
            headerBlock += hpackIndexedField(7);
            headerBlock += hpackLiteralField(4, path);
            headerBlock += hpackLiteralField(1, "localhost");
            return headerBlock;
        }

        /**
         * @brief 拼一个客户端的空 SETTINGS 帧（全部参数取规范初值）
         * @return std::string 完整帧字节
         */
        std::string makeClientSettingsFrame()
        {
            return encodeHttp2SettingsFrame(Http2SettingsPayload{});
        }

        /**
         * @brief 拼一个 SETTINGS ACK 帧（§6.5.3 要求 ACK 负载为空）
         * @return std::string 完整帧字节
         */
        std::string makeSettingsAckFrame()
        {
            return encodeHttp2SettingsFrame(Http2SettingsPayload{.isAcknowledgement = true});
        }

        /**
         * @brief 拼一个 HEADERS 帧（头块在一帧内结束，不带优先级字段）
         * @param streamId 流号
         * @param headerBlock 头块字节
         * @param endStream 是否置 END_STREAM（请求头块带 END_STREAM 表示没有正文）
         * @return std::string 完整帧字节
         */
        std::string makeRequestHeadersFrame(const std::uint32_t streamId, const std::string_view headerBlock, const bool endStream)
        {
            Http2HeadersPayload payload;
            payload.endStream = endStream;
            payload.endHeaders = true;
            payload.headerBlockFragment = std::string(headerBlock);
            return encodeHttp2HeadersFrame(payload, streamId);
        }

        /**
         * @brief 拼一个 DATA 帧
         * @param streamId 流号
         * @param data 应用数据
         * @param endStream 是否置 END_STREAM
         * @return std::string 完整帧字节
         */
        std::string makeDataFrame(const std::uint32_t streamId, const std::string_view data, const bool endStream)
        {
            Http2DataPayload payload;
            payload.endStream = endStream;
            payload.data = std::string(data);
            return encodeHttp2DataFrame(payload, streamId);
        }

        /**
         * @brief 取一条流上收到的完整响应头块（HEADERS 与随后的 CONTINUATION 片段拼起来）
         * @param frames 已收到的帧
         * @param streamId 流号
         * @return std::string 头块字节；没有头块时为空串
         */
        std::string responseHeaderBlock(const std::vector<TestFrame> &frames, const std::uint32_t streamId)
        {
            std::string headerBlock;
            for (const TestFrame &frame: frames)
            {
                if (frame.streamId != streamId || (frame.type != Http2FrameType::Headers && frame.type != Http2FrameType::Continuation))
                {
                    continue;
                }
                headerBlock += frame.payload;
                // END_HEADERS 落在哪一帧，头块就在哪一帧结束（§6.10）
                if ((frame.flags & kHttp2FlagEndHeaders) != 0)
                {
                    break;
                }
            }
            return headerBlock;
        }

        /**
         * @brief 取一条流上收到的全部 DATA 净负载
         * @param frames 已收到的帧
         * @param streamId 流号
         * @return std::string 按到达顺序拼接的正文
         */
        std::string responseDataPayload(const std::vector<TestFrame> &frames, const std::uint32_t streamId)
        {
            std::string responseBody;
            for (const TestFrame &frame: frames)
            {
                if (frame.type == Http2FrameType::Data && frame.streamId == streamId)
                {
                    responseBody += frame.payload;
                }
            }
            return responseBody;
        }

        /**
         * @brief 判断一条流上是否已经收到带 END_STREAM 的帧（响应到此结束）
         * @param frames 已收到的帧
         * @param streamId 流号
         * @return true 已收到末片
         */
        bool hasEndStream(const std::vector<TestFrame> &frames, const std::uint32_t streamId)
        {
            for (const TestFrame &frame: frames)
            {
                if (frame.streamId != streamId || (frame.type != Http2FrameType::Headers && frame.type != Http2FrameType::Data))
                {
                    continue;
                }
                if ((frame.flags & kHttp2FlagEndStream) != 0)
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 用生产解码器解一个响应头块
         * @details 解码器要与服务端编码器的动态表同步演进，因此同一条连接上的多个响应必须按序解。
         * @param decoder 本次连接共用的头块解码器
         * @param headerBlock 头块字节
         * @return std::vector<HpackHeaderField> 解出的头列表
         */
        std::vector<HpackHeaderField> decodeResponseHeaderBlock(HpackDecoder &decoder, const std::string_view headerBlock)
        {
            std::vector<HpackHeaderField> headerFields;
            EXPECT_TRUE(decoder.decode(headerBlock, headerFields)) << "响应头块解不开：" << decoder.errorMessage();
            return headerFields;
        }

        /**
         * @brief 在解出的头列表里找一条头的值
         * @param headerFields 头列表
         * @param name 头名
         * @return std::string 头值；没有该头时为空串
         */
        std::string findHeaderValue(const std::vector<HpackHeaderField> &headerFields, const std::string_view name)
        {
            for (const HpackHeaderField &field: headerFields)
            {
                if (field.name == name)
                {
                    return field.value;
                }
            }
            return {};
        }

        /**
         * @brief 把基类的监听描述符与活跃连接数透出成只读访问器的测试服务器
         * @details 端口由内核分配，只能从监听描述符反查；活跃连接数是「会话是否真的退出」的判据。
         */
        class TestHttp2Server final : public HttpsServer
        {
        public:
            using HttpsServer::HttpsServer;

            /// 监听描述符
            [[nodiscard]] int listenDescriptor() const
            {
                return m_acceptor.fileDescriptor();
            }

            /// 当前挂在连接管理器上的活跃连接数
            [[nodiscard]] std::size_t activeConnectionCount() const
            {
                return m_connectionManager.activeCount();
            }
        };

        /// 大正文响应用例的正文内容：全部是 'L'，便于断言逐字节一致
        constexpr char kLargeResponseFillByte = 'L';

        /**
         * @brief 跑起一台真实 HttpsServer 的夹具（会话侧按 ALPN 分流到 HTTP/2 或 HTTP/1.1）
         * @details 成员顺序即生命周期顺序：循环 → 结果槽 → 服务器 → 主协程任务 → 循环线程。
         *          析构体先等循环线程退出，再收尾服务器，与 HTTP 侧的夹具同一套顺序。
         */
        class RunningHttp2ServerFixture
        {
        public:
            /**
             * @brief 构造并启动服务器（路由在此一次性注册完）
             * @param limits 连接级限额
             * @param sweepInterval 空闲清扫节拍
             * @param parserLimits 解析上限，HTTP/2 路径只用其中的 maximumBodySize
             */
            RunningHttp2ServerFixture(const HttpServerLimits &limits, const std::chrono::milliseconds sweepInterval,
                                      const HttpParserLimits &parserLimits = HttpParserLimits{}) :
                m_loop(),
                m_server(m_loop, Core::InetAddress::localhost(0), kTestCertificatePath.string(), kTestKeyPath.string()),
                m_serverTask(driveStart(m_server, m_startThrew)),
                m_loopThread(m_loop)
            {
                // 限额、清扫节拍与路由都必须在投递 start() 之前落定
                m_server.setLimits(limits);
                m_server.setParserLimits(parserLimits);
                m_server.setIdleCheckInterval(sweepInterval);

                Router &router = m_server.router();
                router.get("/hello", [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                {
                    response.setBody("served-hello");
                    co_return;
                });
                router.post("/echo", [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                {
                    // 回显正文长度：客户端据此断言服务端确实收全了大正文
                    response.setBody(std::to_string(request.body().size()));
                    co_return;
                });
                router.get("/large", [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                {
                    response.setBody(std::string(kLargeResponseByteCount, kLargeResponseFillByte));
                    co_return;
                });
                router.post("/inspect", [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                {
                    // 回显映射结果：:path → uri、:authority → host、版本 → HTTP/2、正文 → body
                    // （请求方法由路由本身钉住：这条路由只注册了 POST，方法没映射对就命不中）
                    response.setBody(std::string(request.path()) + "|" + request.getHeader("host").value_or("<none>") + "|" +
                                     request.httpVersion() + "|" + std::to_string(request.body().size()));
                    co_return;
                });
                router.get("/upgrade", [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                {
                    // 登记 WebSocket 升级：HTTP/2 上没有对应的切换机制，会话应当回 501 而不是静默放行
                    response.upgradeToWebSocket([](WebSocketPeer &) -> Core::Task<>
                    {
                        co_return;
                    });
                    co_return;
                });

                m_loopThread.schedule(m_serverTask);
            }

            ~RunningHttp2ServerFixture()
            {
                // 顺序要紧：先让循环线程停手并退出，再收尾服务器（收尾会销毁挂起的协程帧）
                m_loopThread.join();
                m_server.close();
            }

            RunningHttp2ServerFixture(const RunningHttp2ServerFixture &) = delete;

            RunningHttp2ServerFixture &operator=(const RunningHttp2ServerFixture &) = delete;

            /// 被测服务器本体：观测性用例据此读取统计快照
            [[nodiscard]] TestHttp2Server &server() noexcept
            {
                return m_server;
            }

            /// 服务器是否已进入接受循环
            [[nodiscard]] bool awaitRunning(const std::chrono::milliseconds timeout) const
            {
                return waitForCondition(
                        [this]
                        {
                            return m_server.isRunning();
                        },
                        timeout);
            }

            /// 活跃连接是否已全部退场
            [[nodiscard]] bool awaitConnectionsDrained(const std::chrono::milliseconds timeout) const
            {
                return waitForCondition(
                        [this]
                        {
                            return m_server.activeConnectionCount() == 0;
                        },
                        timeout);
            }

            /// 内核实际分配的监听端口
            [[nodiscard]] std::uint16_t listeningPort() const
            {
                return queryBoundPort(m_server.listenDescriptor());
            }

            /// start() 是否以异常收场（用于把这台用例的失败与「配置没生效」区分开）
            [[nodiscard]] bool startThrew() const
            {
                return m_startThrew.load(std::memory_order_acquire);
            }

        private:
            /**
             * @brief 把 start() 包一层，记录它是否抛异常
             * @param server 被测服务器
             * @param startThrew 输出：是否抛异常
             * @return Core::Task<> 协程，start() 返回后完成
             */
            static Core::Task<> driveStart(TestHttp2Server &server, std::atomic<bool> &startThrew)
            {
                try
                {
                    co_await server.start();
                } catch (...)
                {
                    // 只标记不抛出：用例据此断言「服务器没起来」而不是让测试进程带崩
                    startThrew.store(true, std::memory_order_release);
                }
                co_return;
            }

            Core::EventLoop   m_loop;        ///< 事件循环本体
            std::atomic<bool> m_startThrew{false}; ///< start() 的退出方式，必须先于任务构造
            TestHttp2Server   m_server;      ///< 被测服务器
            Core::Task<>      m_serverTask;  ///< 由 driveStart 产生的主协程队列
            EventLoopThread   m_loopThread;  ///< 承载 run() 的线程，最后构造、最先析构
        };

        /**
         * @brief 一条到 127.0.0.1 的 TLS 客户端：可登记 ALPN、收发 h2 帧，也可发 HTTP/1.1 报文
         *
         * @details 客户端刻意不用框架的 TlsSocket/AsyncSocket：用例只需要「握手、发字节、解帧」，
         *          再引入一套事件循环无助于验证服务端行为。套接字保持非阻塞，握手与读写都用
         *          「SSL_xxx 返回 WANT_* 就轮询重试」的方式推进，因此任何一步都不会把测试线程挂死。
         * @note 客户端不校验服务端证书（仓库夹具是自签证书）；收到的字节同时进两份缓冲：
         *       文本累计（HTTP/1.1 分流用例直接搜它）与帧待解析缓冲（h2 用例喂给生产帧解码器）。
         */
        class TlsHttp2LoopbackClient
        {
        public:
            /**
             * @brief 连接、登记 ALPN 并完成 TLS 握手
             * @param port 服务端监听端口
             * @param offeredProtocolName 登记进 ALPN 的协议名；空串表示完全不提供 ALPN
             */
            TlsHttp2LoopbackClient(const std::uint16_t port, const std::string_view offeredProtocolName)
            {
                m_context.reset(SSL_CTX_new(TLS_client_method()));
                if (m_context == nullptr)
                {
                    return;
                }

                m_descriptor = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
                if (!Platform::FileDescriptor::isValid(m_descriptor))
                {
                    m_descriptor = Platform::FileDescriptor::kInvalid;
                    return;
                }

                sockaddr_in address{};
                address.sin_family      = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port        = htons(port);
                if (::connect(m_descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
                {
                    closeNow();
                    return;
                }

                // 转非阻塞：握手与读写靠轮询推进，用例不会因为对端不响应而永久挂起
                Platform::FileDescriptor::setNonBlocking(m_descriptor);

                m_ssl.reset(SSL_new(m_context.get()));
                if (m_ssl == nullptr || SSL_set_fd(m_ssl.get(), m_descriptor) != 1)
                {
                    m_ssl.reset();
                    closeNow();
                    return;
                }

                if (!offeredProtocolName.empty())
                {
                    // 线上格式是「长度前缀 + 协议名」；返回值 0 表示列表被接受，记下来供用例做前提校验
                    std::vector<unsigned char> wireFormat;
                    wireFormat.push_back(static_cast<unsigned char>(offeredProtocolName.size()));
                    for (const char protocolNameByte: offeredProtocolName)
                    {
                        wireFormat.push_back(static_cast<unsigned char>(protocolNameByte));
                    }
                    m_alpnListAccepted = SSL_set_alpn_protos(m_ssl.get(), wireFormat.data(), static_cast<unsigned int>(wireFormat.size())) == 0;
                }

                m_handshakeDone = performHandshake(kWaitTimeout);
            }

            ~TlsHttp2LoopbackClient()
            {
                closeNow();
            }

            TlsHttp2LoopbackClient(const TlsHttp2LoopbackClient &) = delete;

            TlsHttp2LoopbackClient &operator=(const TlsHttp2LoopbackClient &) = delete;

            /// TLS 握手是否已完成
            [[nodiscard]] bool isHandshakeComplete() const noexcept
            {
                return m_handshakeDone;
            }

            /// ALPN 列表是否被客户端侧接受（不提 ALPN 时恒为 false）
            [[nodiscard]] bool isAlpnListAccepted() const noexcept
            {
                return m_alpnListAccepted;
            }

            /// 客户端视角的 ALPN 协商结果
            [[nodiscard]] std::string selectedAlpnProtocol() const
            {
                if (m_ssl == nullptr)
                {
                    return {};
                }
                const unsigned char *protocolName = nullptr;
                unsigned int protocolNameLength = 0;
                SSL_get0_alpn_selected(m_ssl.get(), &protocolName, &protocolNameLength);
                if (protocolName == nullptr || protocolNameLength == 0)
                {
                    return {};
                }
                return std::string(reinterpret_cast<const char *>(protocolName), protocolNameLength);
            }

            /// 累计收到的全部明文字节（HTTP/1.1 分流用例据此做文本断言）
            [[nodiscard]] const std::string &receivedText() const noexcept
            {
                return m_receivedTextBytes;
            }

            /**
             * @brief 把整段字节写出去
             * @param payload 待发字节
             * @param timeout 写入等待上限
             * @return true 全部字节已交给 OpenSSL
             */
            bool sendBytes(const std::string_view payload, const std::chrono::milliseconds timeout) const
            {
                if (!m_handshakeDone)
                {
                    return false;
                }

                const auto  deadline      = std::chrono::steady_clock::now() + timeout;
                std::size_t writtenLength = 0;

                while (writtenLength < payload.size())
                {
                    const int writeLength = SSL_write(m_ssl.get(), payload.data() + writtenLength,
                                                      static_cast<int>(payload.size() - writtenLength));
                    if (writeLength > 0)
                    {
                        writtenLength += static_cast<std::size_t>(writeLength);
                        continue;
                    }
                    const int errorCode = SSL_get_error(m_ssl.get(), writeLength);
                    if (errorCode != SSL_ERROR_WANT_READ && errorCode != SSL_ERROR_WANT_WRITE)
                    {
                        return false;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return true;
            }

            /**
             * @brief 轮询读直到 isDone 成立
             * @details 每轮先把待解析字节喂进帧解码器（解出的帧按序追加进 frames），再判条件；
             *          帧解码失败视为用例失败（返回 false 并记下原因）。
             * @param frames 输入输出：已解出的帧
             * @param isDone 判定：已收到的帧是否满足目标
             * @param timeout 等待上限
             * @return true 在时限内满足判定
             */
            bool pumpUntil(std::vector<TestFrame> &frames,
                           const std::function<bool(const std::vector<TestFrame> &)> &isDone,
                           const std::chrono::milliseconds timeout)
            {
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (true)
                {
                    drainDecodedFrames(frames);
                    if (isDone(frames))
                    {
                        return true;
                    }
                    if (m_hasDecodeError || std::chrono::steady_clock::now() >= deadline)
                    {
                        return false;
                    }

                    const auto remainingTime = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                    if (!waitForBytes(remainingTime))
                    {
                        // 对端关闭或等待超时：把已经到手的字节解完再给结论
                        drainDecodedFrames(frames);
                        return isDone(frames);
                    }
                }
            }

            /**
             * @brief 轮询读直到观察到对端关闭或连接出错
             * @param timeout 等待上限
             * @return true 在时限内观察到连接已断
             */
            bool waitForClosure(const std::chrono::milliseconds timeout)
            {
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (true)
                {
                    const ReadOutcome outcome = readOnce();
                    if (outcome == ReadOutcome::PeerClosed || outcome == ReadOutcome::Broken)
                    {
                        return true;
                    }
                    if (outcome == ReadOutcome::Data)
                    {
                        continue;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }

            /**
             * @brief 轮询读直到累计文本里出现指定内容
             * @param expectedText 期望出现的文本
             * @param timeout 等待上限
             * @return true 在时限内出现
             */
            bool waitForText(const std::string_view expectedText, const std::chrono::milliseconds timeout)
            {
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (m_receivedTextBytes.find(expectedText) == std::string::npos)
                {
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        break;
                    }
                    const ReadOutcome outcome = readOnce();
                    if (outcome == ReadOutcome::PeerClosed || outcome == ReadOutcome::Broken)
                    {
                        break;
                    }
                    if (outcome == ReadOutcome::Idle)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
                return m_receivedTextBytes.find(expectedText) != std::string::npos;
            }

            /// 关闭本端（尽力发出 close_notify，让服务端看到的是正常关闭）
            void closeNow() noexcept
            {
                if (m_ssl != nullptr)
                {
                    SSL_shutdown(m_ssl.get());
                    m_ssl.reset();
                }
                Platform::FileDescriptor::close(m_descriptor);
                m_descriptor = Platform::FileDescriptor::kInvalid;
            }

        private:
            /**
             * @brief 在时限内推进 SSL_connect 直到完成
             * @param timeout 握手等待上限
             * @return true 握手完成
             */
            bool performHandshake(const std::chrono::milliseconds timeout)
            {
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (true)
                {
                    const int result = SSL_connect(m_ssl.get());
                    if (result == 1)
                    {
                        return true;
                    }
                    const int errorCode = SSL_get_error(m_ssl.get(), result);
                    if (errorCode != SSL_ERROR_WANT_READ && errorCode != SSL_ERROR_WANT_WRITE)
                    {
                        return false;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }

            /**
             * @brief 读一次，把字节同时追加进文本累计与帧待解析缓冲
             * @return ReadOutcome 本次读取的结论
             */
            ReadOutcome readOnce()
            {
                if (!m_handshakeDone)
                {
                    return ReadOutcome::Broken;
                }

                std::array<char, kClientChunkLength> chunkStorage{};
                const int readLength = SSL_read(m_ssl.get(), chunkStorage.data(), static_cast<int>(chunkStorage.size()));
                if (readLength > 0)
                {
                    m_receivedTextBytes.append(chunkStorage.data(), static_cast<std::size_t>(readLength));
                    m_pendingFrameBytes.append(chunkStorage.data(), static_cast<std::size_t>(readLength));
                    return ReadOutcome::Data;
                }

                const int errorCode = SSL_get_error(m_ssl.get(), readLength);
                if (errorCode == SSL_ERROR_ZERO_RETURN)
                {
                    return ReadOutcome::PeerClosed;
                }
                if (errorCode == SSL_ERROR_WANT_READ || errorCode == SSL_ERROR_WANT_WRITE)
                {
                    return ReadOutcome::Idle;
                }
                // 对端未发 close_notify 就断开时是 SYSCALL + 0：同样按「已关闭」而不是「出错」处理
                if (errorCode == SSL_ERROR_SYSCALL && readLength == 0)
                {
                    return ReadOutcome::PeerClosed;
                }
                return ReadOutcome::Broken;
            }

            /**
             * @brief 轮询读直到拿到至少一段新字节
             * @param timeout 等待上限
             * @return true 读到了字节；对端关闭、出错或超时时返回 false
             */
            bool waitForBytes(const std::chrono::milliseconds timeout)
            {
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (true)
                {
                    const ReadOutcome outcome = readOnce();
                    if (outcome == ReadOutcome::Data)
                    {
                        return true;
                    }
                    if (outcome == ReadOutcome::PeerClosed || outcome == ReadOutcome::Broken)
                    {
                        return false;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }

            /**
             * @brief 把待解析字节喂进生产帧解码器，取出已完成的帧
             * @param frames 输入输出：解出的帧按到达顺序追加
             */
            void drainDecodedFrames(std::vector<TestFrame> &frames)
            {
                std::size_t consumedByteCount = 0;
                while (consumedByteCount < m_pendingFrameBytes.size())
                {
                    const Http2FrameDecodeStatus status =
                            m_frameDecoder.parse(m_pendingFrameBytes.data() + consumedByteCount, m_pendingFrameBytes.size() - consumedByteCount);
                    if (status != Http2FrameDecodeStatus::Frame)
                    {
                        // NeedMore：本段字节已被解码器内部收下（半帧由它缓冲）；Error：记下原因，调用方按失败处理
                        if (status == Http2FrameDecodeStatus::Error)
                        {
                            m_hasDecodeError = true;
                            m_decodeErrorMessage = m_frameDecoder.errorMessage();
                        }
                        consumedByteCount = m_pendingFrameBytes.size();
                        break;
                    }
                    consumedByteCount += m_frameDecoder.consumedByteCount();
                    frames.push_back(toTestFrame(m_frameDecoder.takeFrame()));
                }
                m_pendingFrameBytes.clear();
            }

            std::unique_ptr<SSL_CTX, void (*)(SSL_CTX *)> m_context{nullptr, [](SSL_CTX *context)
            {
                SSL_CTX_free(context);
            }};
            std::unique_ptr<SSL, void (*)(SSL *)> m_ssl{nullptr, [](SSL *ssl)
            {
                SSL_free(ssl);
            }};
            int m_descriptor{Platform::FileDescriptor::kInvalid}; ///< 底层 TCP 描述符
            bool m_handshakeDone{false};            ///< TLS 握手是否已完成
            bool m_alpnListAccepted{false};         ///< ALPN 列表是否被客户端侧接受
            bool m_hasDecodeError{false};           ///< 帧解码是否已失败
            std::string m_decodeErrorMessage;       ///< 帧解码失败原因（仅诊断用）
            std::string m_receivedTextBytes;        ///< 累计收到的明文（文本断言用）
            std::string m_pendingFrameBytes;        ///< 尚未喂给帧解码器的字节
            Http2FrameDecoder m_frameDecoder;       ///< 帧解码器：服务端吐出的字节必须能被它解回来
            Platform::Socket::Initialization m_socketInitialization; ///< 保证 Winsock 在本对象存活期间保持初始化
        };
    } // namespace

    /**
     * @brief 钉住：ALPN 提 h2 的客户端走 HTTP/2 循环——200 响应头 + 正文 DATA 末片 END_STREAM，
     *        且同一条连接上的第二条请求照常被服务（连接复用），request-id 生成与回显与 HTTP 侧同口径
     */
    TEST(Http2Session, ServesGetOverAlpnH2EndToEnd)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath)) << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        RunningHttp2ServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环：上界 kWaitTimeout";
        ASSERT_FALSE(fixture.startThrew()) << "HTTPS 服务器 start() 以异常收场";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        TlsHttp2LoopbackClient client(listeningPort, "h2");
        ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";
        ASSERT_TRUE(client.isAlpnListAccepted()) << "客户端未能登记 ALPN 列表";
        EXPECT_EQ(client.selectedAlpnProtocol(), "h2") << "客户端视角没有协商出 h2";

        // 前奏与客户端 SETTINGS：服务端收齐前奏后才会发自己的初始 SETTINGS
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + makeClientSettingsFrame(), kWaitTimeout))
                << "前奏与 SETTINGS 未能写入";

        std::vector<TestFrame> frames;
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<TestFrame> &receivedFrames)
                                     {
                                         return countFrames(receivedFrames, Http2FrameType::Settings) >= 1;
                                     },
                                     kWaitTimeout)) << "没有在时限内收到服务端的初始 SETTINGS";
        // 服务端必须确认客户端的 SETTINGS（§6.5.3）：ACK 帧负载为空
        ASSERT_TRUE(client.sendBytes(makeSettingsAckFrame(), kWaitTimeout));

        // 第一条请求：GET /hello → 200 + served-hello
        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(1U, makeGetRequestHeaderBlock("/hello"), true), kWaitTimeout))
                << "GET 请求未能写入";
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<TestFrame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "第一条请求没有在时限内收到完整响应";

        // 头块解码器与响应编码器成对演进：两条响应必须按序解，否则动态表索引会错位
        HpackDecoder responseDecoder;
        const std::vector<HpackHeaderField> firstResponseHeaders = decodeResponseHeaderBlock(responseDecoder, responseHeaderBlock(frames, 1U));
        EXPECT_EQ(findHeaderValue(firstResponseHeaders, ":status"), "200") << "第一条请求的响应状态码不是 200";
        EXPECT_EQ(responseDataPayload(frames, 1U), "served-hello") << "响应正文与路由返回值不一致";
        const std::string generatedRequestId = findHeaderValue(firstResponseHeaders, "x-request-id");
        EXPECT_TRUE(looksLikeGeneratedRequestId(generatedRequestId))
                << "响应里的 request-id 形态不符合约定（应为 4 位十六进制前缀 + '-' + 16 位十六进制序号）：「" << generatedRequestId << "」";

        // 第二条请求走同一条连接，并带回客户端自带的 request-id：服务端应当原样回显
        const std::string clientRequestId = "h2-trace-id-from-client";
        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(3U, makeGetRequestHeaderBlock("/hello", {{std::string(kRequestIdHeaderName), clientRequestId}}), true),
                                     kWaitTimeout)) << "第二条请求未能写入";
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<TestFrame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 3U);
                                     },
                                     kWaitTimeout)) << "第二条请求没有在时限内收到完整响应";
        const std::vector<HpackHeaderField> secondResponseHeaders = decodeResponseHeaderBlock(responseDecoder, responseHeaderBlock(frames, 3U));
        EXPECT_EQ(findHeaderValue(secondResponseHeaders, ":status"), "200") << "第二条请求的响应状态码不是 200";
        EXPECT_EQ(responseDataPayload(frames, 3U), "served-hello") << "第二条请求的响应正文不符";
        EXPECT_EQ(findHeaderValue(secondResponseHeaders, "x-request-id"), clientRequestId) << "客户端自带的 request-id 没有被原样回显";

        // 统计口径与 HTTP 侧一致：两条请求、两条 2xx，没有 bad request
        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().stats().totalRequestCount >= 2;
                },
                kWaitTimeout)) << "统计未在时限内记下这两条请求";
        const HttpServerStats stats = fixture.server().stats();
        EXPECT_EQ(stats.totalRequestCount, 2u) << "累计请求条数不符";
        EXPECT_EQ(stats.status2xxCount, 2u) << "两条 200 没有计入 2xx";
        EXPECT_EQ(stats.badRequestCount, 0u) << "正常请求被记成了协议错误";

        // 时序纪律：先等客户端读到关闭，再断言会话收口
        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "HTTP/2 会话收口后未从连接管理器摘除";
    }

    /**
     * @brief 钉住：超过初始窗口的 POST 正文只有靠服务端 WINDOW_UPDATE 才能发完
     *
     * @details 正文 100000 字节 > 65535：客户端先发满窗口，之后必须收到连接级与流级的窗口更新
     *          （服务器只在两者都还回来时才能继续收）。用例同时断言确实收到过窗口更新——没有它，
     *          「大正文能收完」就只是碰巧落在窗口之内，用例钉不住流控。
     */
    TEST(Http2Session, ReceivesLargePostBodyThroughWindowUpdates)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath)) << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        RunningHttp2ServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        TlsHttp2LoopbackClient client(listeningPort, "h2");
        ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";
        ASSERT_EQ(client.selectedAlpnProtocol(), "h2");

        std::vector<TestFrame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + makeClientSettingsFrame(), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<TestFrame> &receivedFrames)
                                     {
                                         return countFrames(receivedFrames, Http2FrameType::Settings) >= 1;
                                     },
                                     kWaitTimeout)) << "没有在时限内收到服务端的初始 SETTINGS";
        ASSERT_TRUE(client.sendBytes(makeSettingsAckFrame(), kWaitTimeout));

        // 服务端通告的初始窗口就是客户端能先发的上限；缺省（没通告）时按规范的初值 65535
        const TestFrame *const serverSettingsFrame = findFrame(frames, Http2FrameType::Settings);
        ASSERT_NE(serverSettingsFrame, nullptr);
        const std::uint32_t initialWindowSize = readSettingsInitialWindowSize(*serverSettingsFrame, kHttp2InitialWindowSizeByteCount);

        const std::string postBody(kPostBodyByteCount, 'B');
        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(1U, makePostRequestHeaderBlock("/echo"), false), kWaitTimeout))
                << "POST 请求头未能写入";

        // 发送窗口按连接级与流级分别记账：服务器必须两级都还回来，客户端才继续发（§6.9）
        std::int64_t connectionSendWindow = kHttp2InitialWindowSizeByteCount;
        std::int64_t streamSendWindow = initialWindowSize;
        std::size_t sentByteCount = 0;
        std::size_t processedFrameCount = 0;
        std::size_t windowUpdateFrameCount = 0;
        const auto applyNewWindowUpdates = [&]()
        {
            for (; processedFrameCount < frames.size(); ++processedFrameCount)
            {
                const TestFrame &frame = frames[processedFrameCount];
                if (frame.type != Http2FrameType::WindowUpdate || (frame.streamId != 0U && frame.streamId != 1U))
                {
                    continue;
                }
                ++windowUpdateFrameCount;
                const std::int64_t increment = static_cast<std::int64_t>(readWindowUpdateIncrement(frame.payload));
                if (frame.streamId == 0U)
                {
                    connectionSendWindow += increment;
                } else
                {
                    streamSendWindow += increment;
                }
            }
        };

        const auto sendDeadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (sentByteCount < postBody.size() && std::chrono::steady_clock::now() < sendDeadline)
        {
            const std::int64_t availableWindowByteCount = std::min(connectionSendWindow, streamSendWindow);
            if (availableWindowByteCount > 0)
            {
                const std::size_t chunkByteCount = std::min<std::size_t>(
                        {postBody.size() - sentByteCount, static_cast<std::size_t>(availableWindowByteCount), kHttp2DefaultMaximumFrameSize});
                const bool isLastChunk = sentByteCount + chunkByteCount == postBody.size();
                ASSERT_TRUE(client.sendBytes(makeDataFrame(1U, std::string_view(postBody).substr(sentByteCount, chunkByteCount), isLastChunk), kWaitTimeout))
                        << "正文分片未能写入（已发 " << sentByteCount << " 字节）";
                sentByteCount += chunkByteCount;
                connectionSendWindow -= static_cast<std::int64_t>(chunkByteCount);
                streamSendWindow -= static_cast<std::int64_t>(chunkByteCount);
                continue;
            }

            // 窗口耗尽：必须等服务端的 WINDOW_UPDATE 才能继续——这正是本用例要钉住的行为
            ASSERT_TRUE(client.pumpUntil(frames,
                                         [&windowUpdateFrameCount](const std::vector<TestFrame> &receivedFrames)
                                         {
                                             std::size_t updateCount = 0;
                                             for (const TestFrame &frame: receivedFrames)
                                             {
                                                 if (frame.type == Http2FrameType::WindowUpdate)
                                                 {
                                                     ++updateCount;
                                                 }
                                             }
                                             return updateCount > windowUpdateFrameCount;
                                         },
                                         kWaitTimeout)) << "窗口耗尽后没有收到 WINDOW_UPDATE：大正文停在半途（已发 " << sentByteCount << " 字节）";
            applyNewWindowUpdates();
        }
        ASSERT_EQ(sentByteCount, postBody.size()) << "正文没有在时限内发完";
        ASSERT_GT(windowUpdateFrameCount, 0U) << "全部正文在没有收到任何 WINDOW_UPDATE 的情况下就发完了，用例没有钉住流控";

        // 响应：路由回显正文长度，服务端必须收全 100000 字节
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<TestFrame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "大正文请求没有在时限内收到完整响应";
        HpackDecoder responseDecoder;
        const std::vector<HpackHeaderField> responseHeaders = decodeResponseHeaderBlock(responseDecoder, responseHeaderBlock(frames, 1U));
        EXPECT_EQ(findHeaderValue(responseHeaders, ":status"), "200") << "大正文请求的响应状态码不是 200";
        EXPECT_EQ(responseDataPayload(frames, 1U), std::to_string(postBody.size())) << "服务端收到的正文长度与客户端发出的不一致";

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout));
    }

    /**
     * @brief 钉住：大于对端 MAX_FRAME_SIZE 的响应正文按帧上限拆成多片 DATA，末片带 END_STREAM
     */
    TEST(Http2Session, SplitsLargeResponseBodyByMaximumFrameSize)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath)) << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        RunningHttp2ServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        TlsHttp2LoopbackClient client(listeningPort, "h2");
        ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";

        std::vector<TestFrame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + makeClientSettingsFrame(), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<TestFrame> &receivedFrames)
                                     {
                                         return countFrames(receivedFrames, Http2FrameType::Settings) >= 1;
                                     },
                                     kWaitTimeout)) << "没有在时限内收到服务端的初始 SETTINGS";
        ASSERT_TRUE(client.sendBytes(makeSettingsAckFrame(), kWaitTimeout));

        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(1U, makeGetRequestHeaderBlock("/large"), true), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<TestFrame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "大正文响应没有在时限内收完";

        HpackDecoder responseDecoder;
        const std::vector<HpackHeaderField> responseHeaders = decodeResponseHeaderBlock(responseDecoder, responseHeaderBlock(frames, 1U));
        EXPECT_EQ(findHeaderValue(responseHeaders, ":status"), "200");
        EXPECT_EQ(responseDataPayload(frames, 1U), std::string(kLargeResponseByteCount, kLargeResponseFillByte)) << "拆片重组后的正文与路由返回值不一致";

        std::size_t dataFrameCount = 0;
        std::size_t lastDataFrameIndex = 0;
        for (std::size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex)
        {
            const TestFrame &frame = frames[frameIndex];
            if (frame.type != Http2FrameType::Data || frame.streamId != 1U)
            {
                continue;
            }
            ++dataFrameCount;
            lastDataFrameIndex = frameIndex;
            // 客户端没有调大 SETTINGS_MAX_FRAME_SIZE，因此每片都不得超过规范的初值 16384
            EXPECT_LE(frame.payload.size(), kHttp2DefaultMaximumFrameSize) << "响应正文没有按对端的帧上限拆片";
        }
        EXPECT_GE(dataFrameCount, 3U) << "40000 字节的正文应当按 16384 的帧上限拆成至少 3 片";
        EXPECT_NE((frames[lastDataFrameIndex].flags & kHttp2FlagEndStream), 0U) << "END_STREAM 必须落在末片 DATA 上";

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout));
    }

    /**
     * @brief 钉住：ALPN 只提 http/1.1 的客户端仍走 HTTP/1.1 会话，一次普通 GET 照常得到 200
     */
    TEST(Http2Session, RoutesHttp11ClientToHttp11Session)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath)) << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        RunningHttp2ServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        TlsHttp2LoopbackClient client(listeningPort, "http/1.1");
        ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";
        EXPECT_EQ(client.selectedAlpnProtocol(), "http/1.1") << "客户端视角的 ALPN 结果不是 http/1.1";

        ASSERT_TRUE(client.sendBytes("GET /hello HTTP/1.1\r\nhost: test\r\n\r\n", kWaitTimeout)) << "HTTP/1.1 请求未能写入";
        ASSERT_TRUE(client.waitForText("served-hello", kWaitTimeout))
                << "只提 http/1.1 的客户端没有得到 HTTP/1.1 响应：「" << client.receivedText() << "」";
        EXPECT_NE(client.receivedText().find("HTTP/1.1 200"), std::string::npos) << "响应状态行不是 HTTP/1.1 200";

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout));
    }

    /**
     * @brief 钉住：错误前奏（不是 24 字节 h2 前奏）判 PROTOCOL_ERROR，服务端发 GOAWAY 后收口
     */
    TEST(Http2Session, SendsGoAwayAndClosesOnBadPreface)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath)) << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        RunningHttp2ServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        TlsHttp2LoopbackClient client(listeningPort, "h2");
        ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";

        // 一个 HTTP/1.1 请求替代前奏：第一位就不匹配，服务端必须当场判协议错误
        ASSERT_TRUE(client.sendBytes("GET / HTTP/1.1\r\nhost: test\r\n\r\n", kWaitTimeout)) << "错误前奏未能写入";

        std::vector<TestFrame> frames;
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<TestFrame> &receivedFrames)
                                     {
                                         return countFrames(receivedFrames, Http2FrameType::GoAway) >= 1;
                                     },
                                     kWaitTimeout)) << "错误前奏没有换来 GOAWAY";
        const TestFrame *const goAwayFrame = findFrame(frames, Http2FrameType::GoAway);
        ASSERT_NE(goAwayFrame, nullptr);
        EXPECT_EQ(goAwayFrame->streamId, 0U) << "GOAWAY 必须是连接级帧（流号 0）";
        EXPECT_EQ(readGoAwayErrorCode(goAwayFrame->payload), Http2ErrorCode::ProtocolError) << "GOAWAY 的错误码不是 PROTOCOL_ERROR";

        // 时序纪律：先等到 GOAWAY 这个信号，再断言连接收口
        EXPECT_TRUE(client.waitForClosure(kWaitTimeout)) << "GOAWAY 之后连接没有关闭";
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话收口后未从连接管理器摘除";
    }

    /**
     * @brief 钉住：未收录的方法（:method: BREW）不落进任何业务路由——路径命中回 405 并给出 Allow
     */
    TEST(Http2Session, RejectsUnknownMethodWithoutFallingIntoBusinessRoute)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath)) << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        RunningHttp2ServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        TlsHttp2LoopbackClient client(listeningPort, "h2");
        ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";

        std::vector<TestFrame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + makeClientSettingsFrame(), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<TestFrame> &receivedFrames)
                                     {
                                         return countFrames(receivedFrames, Http2FrameType::Settings) >= 1;
                                     },
                                     kWaitTimeout));
        ASSERT_TRUE(client.sendBytes(makeSettingsAckFrame(), kWaitTimeout));

        // /hello 只注册了 GET：方法未收录时既不能命中 GET 业务，也不能蹭上任何兜底路由
        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(1U, makeMethodRequestHeaderBlock("BREW", "/hello"), true), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<TestFrame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "未收录方法的请求没有在时限内收到响应";

        HpackDecoder responseDecoder;
        const std::vector<HpackHeaderField> responseHeaders = decodeResponseHeaderBlock(responseDecoder, responseHeaderBlock(frames, 1U));
        EXPECT_EQ(findHeaderValue(responseHeaders, ":status"), "405") << "未收录方法应当回 405（路径命中、方法不被允许）";
        EXPECT_NE(findHeaderValue(responseHeaders, "allow").find("GET"), std::string::npos) << "405 应当带 Allow: GET";

        // 连接不该因为一条未收录方法就收口：同一个连接上再发一条 GET 仍然可用
        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(3U, makeGetRequestHeaderBlock("/hello"), true), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<TestFrame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 3U);
                                     },
                                     kWaitTimeout)) << "405 之后连接不再可用，正常请求没有响应";
        EXPECT_EQ(responseDataPayload(frames, 3U), "served-hello") << "405 之后的正常请求没有被服务";
    }

    /**
     * @brief 钉住：请求正文超过 HttpParserLimits::maximumBodySize 时回 413 并计入 badRequestCount
     */
    TEST(Http2Session, RejectsOversizeRequestBodyWith413)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath)) << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        HttpParserLimits parserLimits;
        parserLimits.maximumBodySize = 64;

        RunningHttp2ServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100}, parserLimits);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        TlsHttp2LoopbackClient client(listeningPort, "h2");
        ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";

        std::vector<TestFrame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + makeClientSettingsFrame(), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<TestFrame> &receivedFrames)
                                     {
                                         return countFrames(receivedFrames, Http2FrameType::Settings) >= 1;
                                     },
                                     kWaitTimeout));
        ASSERT_TRUE(client.sendBytes(makeSettingsAckFrame(), kWaitTimeout));

        // 200 字节正文 > 上限 64：服务端必须停止缓冲并按 413 应答（与 HTTP 侧同一口径）
        const std::string oversizeBody(200, 'O');
        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(1U, makePostRequestHeaderBlock("/echo"), false), kWaitTimeout));
        ASSERT_TRUE(client.sendBytes(makeDataFrame(1U, oversizeBody, true), kWaitTimeout));

        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<TestFrame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "超限请求没有在时限内收到响应";
        HpackDecoder responseDecoder;
        const std::vector<HpackHeaderField> responseHeaders = decodeResponseHeaderBlock(responseDecoder, responseHeaderBlock(frames, 1U));
        EXPECT_EQ(findHeaderValue(responseHeaders, ":status"), "413") << "正文超限应当回 413";
        EXPECT_EQ(responseDataPayload(frames, 1U), "Payload Too Large") << "413 的正文与 HTTP 侧不一致";

        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().stats().badRequestCount >= 1;
                },
                kWaitTimeout)) << "超限请求没有计入 badRequestCount";
        EXPECT_EQ(fixture.server().stats().totalRequestCount, 0u) << "超限的请求不该计入已处理的请求条数";
    }

    /**
     * @brief 钉住：HTTP/2 上登记了 WebSocket 升级的响应回 501（等价机制是 RFC 8441 的扩展 CONNECT，本片不做）
     */
    TEST(Http2Session, Answers501WhenWebSocketUpgradeIsRequested)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath)) << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        RunningHttp2ServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        TlsHttp2LoopbackClient client(listeningPort, "h2");
        ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";

        std::vector<TestFrame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + makeClientSettingsFrame(), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<TestFrame> &receivedFrames)
                                     {
                                         return countFrames(receivedFrames, Http2FrameType::Settings) >= 1;
                                     },
                                     kWaitTimeout));
        ASSERT_TRUE(client.sendBytes(makeSettingsAckFrame(), kWaitTimeout));

        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(1U, makeGetRequestHeaderBlock("/upgrade"), true), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<TestFrame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "升级请求没有在时限内收到响应";

        HpackDecoder responseDecoder;
        const std::vector<HpackHeaderField> responseHeaders = decodeResponseHeaderBlock(responseDecoder, responseHeaderBlock(frames, 1U));
        EXPECT_EQ(findHeaderValue(responseHeaders, ":status"), "501") << "HTTP/2 上的 WebSocket 升级应当明确回 501，而不是静默当成普通响应";

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout));
    }

    /**
     * @brief 钉住：HEAD 响应只发头部、不发正文，但 content-length 仍按完整正文补齐（RFC 9110 §9.1）
     */
    TEST(Http2Session, AnswersHeadWithoutBodyButWithContentLength)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath)) << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        RunningHttp2ServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        TlsHttp2LoopbackClient client(listeningPort, "h2");
        ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";

        std::vector<TestFrame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + makeClientSettingsFrame(), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<TestFrame> &receivedFrames)
                                     {
                                         return countFrames(receivedFrames, Http2FrameType::Settings) >= 1;
                                     },
                                     kWaitTimeout));
        ASSERT_TRUE(client.sendBytes(makeSettingsAckFrame(), kWaitTimeout));

        // HEAD /hello：路由器按 GET 的处理器执行（RFC 9110 §9.1），会话必须剥掉正文
        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(1U, makeMethodRequestHeaderBlock("HEAD", "/hello"), true), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<TestFrame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "HEAD 请求没有在时限内收到响应";

        HpackDecoder responseDecoder;
        const std::vector<HpackHeaderField> responseHeaders = decodeResponseHeaderBlock(responseDecoder, responseHeaderBlock(frames, 1U));
        EXPECT_EQ(findHeaderValue(responseHeaders, ":status"), "200");
        EXPECT_EQ(responseDataPayload(frames, 1U), "") << "HEAD 响应不得携带正文字节";
        EXPECT_EQ(findHeaderValue(responseHeaders, "content-length"), std::to_string(std::string_view("served-hello").size()))
                << "content-length 应当是完整正文长度的十进制文本";

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout));
    }

    /**
     * @brief 钉住：伪头与头部按 HTTP/1.1 语义映射到 HttpRequest（:path → uri、:authority → host、
     *        版本 → HTTP/2、正文 → body），方法原文不认识时落到 UNKNOWN 由路由器处置
     */
    TEST(Http2Session, MapsTheRequestOntoHttpRequestFields)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath)) << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        RunningHttp2ServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        TlsHttp2LoopbackClient client(listeningPort, "h2");
        ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";

        std::vector<TestFrame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + makeClientSettingsFrame(), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<TestFrame> &receivedFrames)
                                     {
                                         return countFrames(receivedFrames, Http2FrameType::Settings) >= 1;
                                     },
                                     kWaitTimeout));
        ASSERT_TRUE(client.sendBytes(makeSettingsAckFrame(), kWaitTimeout));

        // /inspect 只注册了 POST：命中本身就证明 :method 映射到了 POST，而不是落到 UNKNOWN
        ASSERT_TRUE(client.sendBytes(makeRequestHeadersFrame(1U, makePostRequestHeaderBlock("/inspect"), false), kWaitTimeout));
        ASSERT_TRUE(client.sendBytes(makeDataFrame(1U, "hello", true), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<TestFrame> &receivedFrames)
                                     {
                                         return hasEndStream(receivedFrames, 1U);
                                     },
                                     kWaitTimeout)) << "映射用例的请求没有在时限内收到响应";

        HpackDecoder responseDecoder;
        const std::vector<HpackHeaderField> responseHeaders = decodeResponseHeaderBlock(responseDecoder, responseHeaderBlock(frames, 1U));
        EXPECT_EQ(findHeaderValue(responseHeaders, ":status"), "200") << "POST 映射失败（方法或路径没有落到业务路由上）";
        EXPECT_EQ(responseDataPayload(frames, 1U), "/inspect|localhost|HTTP/2|5")
                << "映射结果不符：:path → uri、:authority → host、版本 HTTP/2、正文按原样追加";

        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout));
    }

    /**
     * @brief 钉住：h2 连接同样受空闲清扫约束——建连后一个请求都不发，按 idleTimeout 被收口并计入超时计数
     */
    TEST(Http2Session, ClosesIdleHttp2ConnectionAndCountsTimeout)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath)) << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        HttpServerLimits limits;
        limits.idleTimeout  = std::chrono::milliseconds{300};
        limits.readTimeout  = std::chrono::seconds{10};
        limits.writeTimeout = std::chrono::seconds{10};

        RunningHttp2ServerFixture fixture(limits, std::chrono::milliseconds{30});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        TlsHttp2LoopbackClient client(listeningPort, "h2");
        ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";

        // 前奏与 SETTINGS 完成协商，此后一个请求都不发：连接停在空闲相位
        std::vector<TestFrame> frames;
        ASSERT_TRUE(client.sendBytes(std::string(kHttp2ConnectionPreface) + makeClientSettingsFrame(), kWaitTimeout));
        ASSERT_TRUE(client.pumpUntil(frames,
                                     [](const std::vector<TestFrame> &receivedFrames)
                                     {
                                         return countFrames(receivedFrames, Http2FrameType::Settings) >= 1;
                                     },
                                     kWaitTimeout)) << "没有在时限内收到服务端的初始 SETTINGS";

        EXPECT_TRUE(client.waitForClosure(kWaitTimeout))
                << "空闲 h2 连接未被清扫协程收口：上界 kWaitTimeout（idleTimeout 300ms + 清扫节拍 30ms）";
        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().stats().timeoutClosedCount >= 1;
                },
                kWaitTimeout)) << "被超时收口的 h2 连接没有计入 timeoutClosedCount";
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话收口后未从连接管理器摘除";
    }
} // namespace AsynGyanis::Net
