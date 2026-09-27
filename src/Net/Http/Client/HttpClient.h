/**
 * @file HttpClient.h
 * @brief 出站 HTTP 客户端与 URL 拆解：一次请求一条连接，明文与 https 两条路都支持
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "Core/Coroutine/Task.h"
#include "Net/Http/Client/HttpOutboundConnectionPool.h"
#include "Net/Http/Client/OutboundCircuitBreaker.h"
#include "Net/Http/Client/HttpResponseParser.h"
#include "Net/Http/HttpBodyChunk.h"
#include "Net/Http/TraceContext.h"
namespace AsynGyanis::Core
{
    class EventLoop;
}
namespace AsynGyanis::Core
{
    class TlsContext;
    struct TlsPolicy;
} // namespace AsynGyanis::Core

namespace AsynGyanis::Net
{
    class HttpCookieJar;

    /// 一条头部字段：名与值都按对端/调用方给出的原文留着，客户端不做大小写折叠
    using HttpClientHeaderField = std::pair<std::string, std::string>;

    /// HTTP 客户端响应
    struct HttpClientResponse
    {
        int                                statusCode{0}; ///< 状态码；0 表示没拿到响应（连接或 TLS 失败）
        std::string                        reasonPhrase;  ///< 状态行里的原因短语
        std::vector<HttpClientHeaderField> headers;       ///< 头部字段，按收到的顺序原样留着
        std::string                        body;          ///< 正文；chunked 已按块拼回原样
    };
    /**
     * @brief 一次出站请求的参数
     * @details body 与 contentType 是视图：请求要到协程挂起之后才写进套接字，调用方必须让这些字节活到
     *          co_await 返回。headers 只准添附加字段——Host、Content-Length、Connection 由客户端按这次
     *          请求的实际情况写，占用它们等于让调用方自己撕裂请求行，因此连同含 CR/LF 的名字与值一起拒绝。
     */
    /**
     * @brief 响应正文的接收口：按到达批次把正文交给调用方，交完才去读下一批通路字节
     *
     * @details 这就是背压的落点——本口没返回，客户端就不会再读，TCP 窗口与 h2/h3 的流控因此自己闭合。
     * @param head 已收齐的响应头部（状态码与头部可信；**只在本次调用内有效**，要留就自己抄下来）
     * @param batch 本批正文（**只在本次调用内有效**；`isLastBatch` 为真时可为空，表示零长收尾）
     * @param isLastBatch 是否最后一批：Content-Length 收满、分块终止块读完、或对端收线
     * @return true 还要下一批；false 就此收口（不再读正文，这条连接当场关闭、不还池）
     */
    using HttpResponseBodyReceiver = std::function<Core::Task<bool>(const HttpResponseInfo &head, std::string_view batch, bool isLastBatch)>;

    struct HttpClientRequest
    {
        std::string                        method{"GET"}; ///< 请求方法，原样写进请求行；HEAD 的应答按 RFC 9112 §6.3 在头块之后结束
        std::string_view                   body{};        ///< 正文；为空时不写 Content-Length，也不写 Content-Type
        std::string_view                   contentType{}; ///< 正文媒体类型，只随非空正文一起写出
        std::vector<HttpClientHeaderField> headers{};     ///< 附加头部，按给出的顺序上线
        /**
         * @brief 流式正文的来源：一段一段交出，写完才算正文结束
         * @details 填了它就忽略 body（contentType 照旧生效）。两种正文写法同时给属于用法错误，当场拒绝。
         *          上线的形状按通路而定：HTTP/1.1 是 `Transfer-Encoding: chunked`（不再写
         *          Content-Length），HTTP/2 是分帧的 DATA；两条都是**拉一段、发一段**，所以传一个大文件
         *          时内存里同时只有一份分段，而不是整份先攒进一个字符串。
         * @note 整体时限（requestTimeout）覆盖「生产正文」这一段：来源自己按住不放，到点就是一条失败
         *       请求，不会把循环挂住
         */
        HttpBodyChunkSource bodySource{};

        /**
         * @brief 响应正文的接收口：设了就「边到边交」，整条响应的正文不再攒在本端内存里
         * @details 签名的读法见 `HttpResponseBodyReceiver`。设了这个口子时，交回来的
         *          `HttpClientResponse::body` 是**空的**（字节都交给了接收口），成败仍看返回的响应
         *          是否为空；`isLastBatch` 为真才算正文走到头。
         * @note 整体时限（requestTimeout）照旧覆盖到正文交完为止：对端按住不送、或本口自己按住不放，
         *       到点就是一条失败请求。本客户端没有「不限时限」这一档，要收一条不结束的流
         *       （`text/event-stream` 那一类），就把时限给到你能接受的上限，并在需要时从本口返回
         *       false 主动收口。
         */
        HttpResponseBodyReceiver responseBodyReceiver{};

        /**
         * @brief 要上线的链路上下文：填了就由本客户端按它写出 traceparent 头部
         * @details 装的是**本端这一跳的上下文**——通常就是当前那一节的 `Span::identifiers()`，
         *          或入站请求归一化后读到的那份（`extractTraceContext()`）。标识按原样渲染，
         *          本客户端不重新生成：谁在跟踪这一跳，谁的标识才作数。
         * @note 不填（默认）时行为与从前一致：headers 里已有 traceparent 就原样发出，没有就不发。
         *          出站链路记录是可选的，不记链路的调用方不必先学一套上下文模型。
         * @note 与 headers 里手写的 traceparent 同时给出属于用法错误，当场拒绝：一处请求只能有一个
         *          上级，替调用方挑一个就是把两份意图混成一条上线头部。
         * @see Net::Traceparent, Net::Span::identifiers()
         */
        std::optional<TraceIdentifiers> traceContext{};
    };
    /**
     * @brief 拆开的请求 URL
     */
    struct ParsedUrl
    {
        std::string scheme{"http"}; ///< 协议，只有 "http" 与 "https" 两种取值（识别大小写无关，存下来已归一化成小写）
        std::string host;           ///< 主机名或 IP 字面量，不做百分号解码；IPv6 已去掉方括号（发 Host 头时按规范加回）
        uint16_t    port{80};       ///< 端口；URL 里没写时 https 取 443、其余取 80
        std::string path{"/"};      ///< 请求路径，含查询串；没写路径时为 "/"
    };

    /**
     * @brief 拆一个 http(s) URL
     * @details 只做拆分，不改写：不百分号解码，也不接受空白与控制字符（那会撕裂请求行）。
     *          写错的端口不会回落到 80、缺协议名也不会补成 http——两者都是「猜一个」，
     *          而猜错的后果是把一段本应加密的流量静默按明文发出去。
     * @param url 形如 http(s)://host[:port]/path；协议名必须写出来且大小写无关，IPv6 主机必须写成 "[::1]:8080"
     * @return ParsedUrl 拆好的协议、主机、端口与路径
     * @throws Base::InvalidArgumentException URL 含空白或控制字符、没有协议名、协议不是 http/https、
     *         没有主机、端口不是 1..65535 的十进制数、方括号没闭合，或 IPv6 字面量没加方括号
     */
    [[nodiscard]] ParsedUrl parseUrl(std::string_view url);
    /**
     * @brief 出站 HTTP 客户端
     * @details 每次请求新建一条连接，完成后关闭（https 走 TLS，并校验服务端证书与主机名）。
     */
    class HttpClient
    {
    public:
        /// 单次请求的默认整体时限（连接、握手、发送、收完响应四段之和）
        static constexpr std::chrono::milliseconds kDefaultRequestTimeout{30000};

        /**
         * @brief 按给定参数发一次请求，收完整个响应
         * @param loop 所属事件循环（提供套接字与定时器）
         * @param url 目标地址，形如 http(s)://host[:port]/path，畸形写法与 parseUrl 同一口径拒绝
         * @param request 方法、正文与附加头部。按值收下，但其中的视图须活到本次 co_await 完成
         * @param requestTimeout 整体时限，语义同 get()
         * @return std::expected<HttpClientResponse, std::string> 成功交出响应；失败交出中文原因，
         *         写清断在哪一段（解析地址 / 建立连接 / TLS 握手 / 写出请求 / 读响应）
         * @throws Base::InvalidArgumentException URL 畸形，或附加头部名字为空、含 CR/LF 控制字符，
         *         或占用了 Host、Content-Length、Connection
         */
        [[nodiscard]] static Core::Task<std::expected<HttpClientResponse, std::string>> send(Core::EventLoop &loop, std::string_view url, HttpClientRequest request,
                                                                                             std::chrono::milliseconds requestTimeout = kDefaultRequestTimeout);

        /**
         * @brief 发起 GET 请求
         * @param loop 所属事件循环（提供套接字与定时器）
         * @param url 目标地址，形如 http(s)://host[:port]/path
         * @param requestTimeout 整体时限：到时直接掐断连接并返回空响应，避免对端只连不应答时
         *        把调用方永远挂住。TLS 握手、写出、收完响应之外，**TCP 连接**也在其中——这一段的时限
         *        是补上的：Windows 的完成端口后端上「连不上」并没有可写事件可等（实测系统 2 秒内就把
         *        连接拒了，挂在可写上的协程却永远等不到那一次唤醒），交给内核的 SYN 重试兜底等于让
         *        调用方无限期停在这里。只有域名解析不在其中，那一步由系统解析器兜底
         * @return std::unique_ptr<HttpClientResponse> 响应；失败（含超时）返回空
         * @note 失败原因会记进 ERROR 日志；要按原因分支就走 send()。需要附加头部也用 send()
         */
        static Core::Task<std::unique_ptr<HttpClientResponse>> get(Core::EventLoop &loop, std::string_view url, std::chrono::milliseconds requestTimeout = kDefaultRequestTimeout);

        /**
         * @brief 发起 POST 请求（正文 Content-Type: application/x-www-form-urlencoded）
         * @param loop 所属事件循环
         * @param url 目标地址
         * @param contentType 正文媒体类型
         * @param body 正文
         * @param requestTimeout 整体时限，语义同 get()
         * @return std::unique_ptr<HttpClientResponse> 响应；失败（含超时）返回空
         * @note 失败原因会记进 ERROR 日志；要附加头部或按原因分支就走 send()
         */
        static Core::Task<std::unique_ptr<HttpClientResponse>> post(Core::EventLoop &loop, std::string_view url, std::string_view contentType, std::string_view body,
                                                                    std::chrono::milliseconds requestTimeout = kDefaultRequestTimeout);

        /**
         * @brief 建一个带空闲连接池的客户端：同一目标主机的连续请求复用一条 keep-alive 连接
         * @details 静态的 get()/post() 一次一条连接、收尾就关；对同一台主机反复出站时，每次都要重做
         *          DNS、TCP 与 TLS 握手。本实例把这些摊掉：用完且对端没声明 close 的连接还回池里，
         *          下次同主机同端口的请求先复用它。
         * @param loop 所属事件循环。实例连同它的池只在这条循环上用——协程挂起期间被别的线程驱动会
         *        踩坏套接字状态，因此本对象不跨线程共享（与框架里每条连接归属一个循环的约定同一口径）
         * @param poolConfig 池的规模参数：空闲多久收口、每个目标最多留几条、一条响应正文的字节上限
         * @note 这一支用默认档的出站 TLS：等价于把默认构造的 Core::TlsPolicy 交给下面那一支，
         *       即「按系统信任库校验对端证书与主机名、OpenSSL 默认套件与曲线」
         * @note 静态的 get()/post()/send() 那一路仍走进程级默认上下文（它们没有承载策略的地方）
         * @note 本头文件刻意只前向声明 TlsPolicy/TlsContext：把 OpenSSL 的伞文件拉进来会让
         *       所有包含方（Middleware.h、HttpSession.h…）里的 std::numeric_limits<T>::max()
         *       被 windows 的宏炸开——那两个默认参数因此拆成两支构造，而不是写 "= {}"
         */
        explicit HttpClient(Core::EventLoop &loop, HttpOutboundConnectionPool::Config poolConfig = {});

        /**
         * @brief 收尾本客户端的连接池
         * @details 声明在此、定义在实现文件：成员里那份 TLS 上下文对本头文件只是前向声明，
         *          析构若由编译器在别处合成，就要在每个持有 HttpClient 的翻译单元里删除一个
         *          不完整类型（MSVC 直接拒）。out-of-line 一份就把它挡住，也让 OpenSSL 留在实现侧
         */
        ~HttpClient();

        /**
         * @brief 同上，外加一份出站 TLS 策略
         * @param loop 所属事件循环，口径同上面那支
         * @param poolConfig 池的规模参数
         * @param tlsPolicy 出站 TLS 的策略与信任库（版本区间、套件、曲线、CA 文件/目录、校验深度、
         *        票据开关）；默认构造即「按系统信任库校验对端」。本实例的 HTTPS 请求都用它，
         *        不再与其它 HttpClient 实例共用一个进程级上下文——共用时一个实例的策略会把别人的
         *        握手档位一起改掉
         * @throws Core::CoreException 策略里某一项被当前 OpenSSL 拒绝（版本区间、套件列表、曲线、
         *         CA 信任库）：构造期就抛，比每条请求都拿到一句「TLS 上下文创建失败」好查
         */
        explicit HttpClient(Core::EventLoop &loop, HttpOutboundConnectionPool::Config poolConfig, const Core::TlsPolicy &tlsPolicy);

        /**
         * @brief 给出站连接带上自己的客户端证书（双向 TLS 的出站侧）
         * @details 服务端要求出示证书时（HttpsServer::setClientCertificateRequired(true)），
         *          本端不配身份就连不上。证书与私钥装在**本实例的上下文**上，之后每条 HTTPS 连接
         *          都会带上它（握手时按对端请求的 CA 选链，OpenSSL 负责挑）。
         * @param certificateFile 客户端证书（PEM，可含链）
         * @param keyFile 私钥（PEM）
         * @return true 已装载；false 加载失败（文件缺失、格式不对、与私钥不配对），本端保持原状态
         * @note 与信任库那几项不同，身份不是策略字段：它是「这台客户端是谁」，与「怎么握手」分开，
         *       也便于只换证书不动其它 TLS 配置
         * @note 路径会被记下来：开了 HTTP/3 时 QUIC 连接按同样的路径带身份，双向 TLS 的两侧
         *       不会一条通道带、另一条不带
         */
        bool setClientCertificate(const std::string &certificateFile, const std::string &keyFile);

        /**
         * @brief 发一次 GET，能复用就复用空闲连接
         * @param url 目标地址，口径同静态的 get()
         * @param requestTimeout 整体时限，语义同静态的 get()：连接、握手、发送、收完响应四段之和
         * @return std::unique_ptr<HttpClientResponse> 响应；失败（含超时）返回空
         * @note 复用的那条连接如果对端已经关掉，本次请求会**自动重开一条再来一次**——这是 keep-alive
         *       的固有竞态（对端随时可以收掉空闲连接），不是失败。两类情形不重发：读到过响应字节
         *       （那已经是「响应本身有问题」，重来不换一个答案）；请求已整个写上通路而方法不在幂等
         *       集合里（GET/HEAD/OPTIONS/PUT/DELETE/TRACE，RFC 9110 §9.2.2；RFC 9112 §9.3.2 给的自动
         *       重试许可也只覆盖幂等方法）。HTTP/1.1 与 HTTP/2 两条通路用同一条判据
         */
        [[nodiscard]] Core::Task<std::unique_ptr<HttpClientResponse>> get(std::string_view url, std::chrono::milliseconds requestTimeout = kDefaultRequestTimeout);

        /**
         * @brief 发一次 POST，复用与重试口径同 get()
         * @param url 目标地址
         * @param contentType 正文媒体类型
         * @param body 正文
         * @param requestTimeout 整体时限，语义同 get()
         * @return std::unique_ptr<HttpClientResponse> 响应；失败（含超时）返回空
         */
        [[nodiscard]] Core::Task<std::unique_ptr<HttpClientResponse>> post(std::string_view url, std::string_view contentType, std::string_view body,
                                                                           std::chrono::milliseconds requestTimeout = kDefaultRequestTimeout);

        /// 当前空闲、可被复用的 HTTP/1.1 连接条数
        [[nodiscard]] std::size_t idleConnectionCount() const noexcept;

        /**
         * @brief 当前留着待命的 HTTP/2 连接条数（一台主机最多一条）
         * @details 与上面那条分开数是对的：h1 的连接按「一次一个请求」出租，h2 的连接按流复用，
         *          两者对同一次突发给出的答案本来就不同。
         * @return std::size_t 池里活着的 h2 连接条数
         */
        [[nodiscard]] std::size_t idleHttp2ConnectionCount() const noexcept;

        /**
         * @brief 最忙的那条待命 h2 连接上同时在途的流数（本端实测到的复用度）
         * @details 与上面的连接条数一起读：「一条连接 + 复用度二」才是并发请求共用同一条连接的证据
         * @return std::size_t 各条连接在途流数的最大值；池里没货返回 0
         */
        [[nodiscard]] std::size_t http2MaximumInFlightStreamCount() const noexcept;

        /// 收掉所有空闲连接：在途请求用的连接不受影响
        void closeIdleConnections() noexcept;

        /**
         * @brief 挂上一个 Cookie 存储罐，让这条实例自动收发放给的 Cookie。
         * @details 装上之后：每次请求前按目标主机、连接是否加密与请求路径拼一条 Cookie 头发出去；
         *          响应里的 Set-Cookie 全部收回罐子（作用域规则见 `HttpCookieJar`）。
         * @details 调用方自己在 `request.headers` 里写了 cookie 头时**以调用方为准**，本方法不再补——
         *          那等于静默改写他明确给出的头部。多个实例共用一个罐子即可共享会话（罐子本身可跨线程）。
         * @note 只有实例这一路（`get`/`post`/带池的 `send`）会用到罐子；静态的 `send()` 不带任何状态，
         *       因此也没有罐子可挂
         * @param cookieJar 罐子；传空指针摘掉（默认就是空，不挂罐子时行为与之前完全一致）
         */
        void setCookieJar(std::shared_ptr<HttpCookieJar> cookieJar) noexcept;

        /**
         * @brief 取回当前挂着的 Cookie 存储罐。
         * @return std::shared_ptr<HttpCookieJar> 没挂则返回空指针
         */
        [[nodiscard]] std::shared_ptr<HttpCookieJar> cookieJar() const noexcept;

        /**
         * @brief 换上（或摘掉）出站熔断器。
         * @details 实例构造时**自带**一个默认配置的熔断器：同一端点连续 5 次失败即开闸 30 秒，
         *          开闸期间对新请求直接给出失败原因，不再付 DNS、TCP 与 TLS 握手的钱——上游整个塌掉时
         *          这笔钱原本每个请求都要重付一遍，而调用方看到的只是「超时」。
         * @param circuitBreaker 熔断器；传空指针即关掉这条保护，退回「每次都试」的行为
         */
        void setCircuitBreaker(std::shared_ptr<OutboundCircuitBreaker> circuitBreaker) noexcept;

        /**
         * @brief 取回当前生效的熔断器。
         * @return std::shared_ptr<OutboundCircuitBreaker> 已被摘掉时返回空指针
         */
        [[nodiscard]] std::shared_ptr<OutboundCircuitBreaker> circuitBreaker() const noexcept;

        /**
         * @brief 打开或关掉本客户端的 HTTP/3 出站通路（默认关）
         * @details 开关而不是自动协商：本框架不缓存 Alt-Svc，调用方要的是「这个 URL 会走哪条通路」
         *          的确定性。打开之后每条 https 出站先试 h3——池里有待命链路就直接复用，没有就探
         *          一条；探不通、起步没走完或这条流没答话，都**回落到 TCP**（h2 还是 h1 由 ALPN 定），
         *          调用方拿到的响应与协议无关。
         * @param isEnabled true 允许走 h3；false 恒走 TCP，与打开之前的行为逐字一致
         * @note 四条边界：①明文 `http://` 不走 h3（QUIC 的 TLS 是强制的）；②带 `bodySource` 的流式
         *       上传不走 h3——h3 的出站入口只收整份正文，没有流式出口，这条请求改走 TCP 而不是把
         *       来源整块缓冲下来（那会悄悄改掉这条请求的内存账）；③静态的 `get()/post()/send()`
         *       那一支没有承载开关与 TLS 策略的地方，恒走 TCP；④**一个端点在本客户端的存活期内只探
         *       一次**：探败过就记下、此后直接走 TCP。少了这条记账，一批并发请求会各付一次探测时限
         *       （它们还被同一端点的建连资格串在彼此后面）；服务端「后来才开 h3」要重新建一个客户端才认。
         */
        void setHttp3Enabled(bool isEnabled) noexcept;

        /// 本客户端当前是否允许走 HTTP/3
        [[nodiscard]] bool isHttp3Enabled() const noexcept;

        /**
         * @brief 池里留着的 h3 链路条数（一台主机最多一条）
         * @return std::size_t 活着的 h3 链路条数
         */
        [[nodiscard]] std::size_t idleHttp3LinkCount() const noexcept;

        /**
         * @brief 最忙的那条待命 h3 链路上同时在途的流数
         * @details 与上面的条数一起读才是复用的证据，口径同 h2 那一对。
         * @return std::size_t 各条链路在途流数的最大值；池里没货返回 0
         */
        [[nodiscard]] std::size_t http3MaximumInFlightStreamCount() const noexcept;

        /**
         * @brief 走本实例的连接池（以及挂上的 Cookie 罐）发一次完整请求。
         * @details 实例这一向原先只有 `get`/`post` 两个便利入口，带自定义头部的请求只能退回静态
         *          `send()`——那条路不带池，也就把连接复用与 Cookie 罐一起丢了。这里补上的是同一个
         *          形状的实例版本，失败口径与 `get`/`post` 一致（空指针 + ERROR 日志）。
         * @param url 目标地址，口径同 parseUrl
         * @param request 方法、正文、媒体类型与附加头部；其中视图须活到本次 co_await 完成
         * @param requestTimeout 整体时限（连接、握手、发送、收完响应四段之和）
         * @return std::unique_ptr<HttpClientResponse> 响应；失败返回空
         * @throws Base::InvalidArgumentException URL 畸形或头部写法会撕裂请求行
         */
        [[nodiscard]] Core::Task<std::unique_ptr<HttpClientResponse>> send(std::string_view url, const HttpClientRequest &request,
                                                                           std::chrono::milliseconds requestTimeout = kDefaultRequestTimeout);

    private:
        /**
         * @brief 走这条实例的连接池发一次请求，失败口径同静态的 get()/post()
         * @details 静态那一路不带池（一次一条连接、要原因就走 send()），这一路带池；两条都把失败原因
         *          记进 ERROR 日志，调用方只看到空响应。
         * @param url 目标地址，口径同 parseUrl
         * @param request 方法、正文、媒体类型与附加头部
         * @param requestTimeout 整体时限（连接、握手、发送、收完响应四段之和）
         * @return std::unique_ptr<HttpClientResponse> 响应；失败（含超时）返回空
         */
        [[nodiscard]] Core::Task<std::unique_ptr<HttpClientResponse>> sendPooled(std::string_view url, const HttpClientRequest &request, std::chrono::milliseconds requestTimeout);

        Core::EventLoop           *m_loop{nullptr}; ///< 所属事件循环（不拥有）
        HttpOutboundConnectionPool m_pool;          ///< 本客户端的空闲连接池
        /// 本实例自己的 TLS 上下文（客户端角色）：策略、信任库与客户端证书都装在这里。
        /// 每个实例一份而不是共用进程级那一份：共用时一个实例的策略会把别人的握手档位一起改掉
        std::unique_ptr<Core::TlsContext> m_clientTls;
        /// 上面那份策略的副本，用指针留着是为了让本头文件继续只前向声明 TlsPolicy（见构造函数的说明）。
        /// 副本存在的理由：h3 的 QUIC 连接要**自己建**一份 TLS 上下文，它吃的是策略而不是现成的上下文
        std::unique_ptr<Core::TlsPolicy> m_tlsPolicy;
        /// setClientCertificate() 给过的身份路径：TCP 侧已经装在 m_clientTls 上，
        /// QUIC 侧建连接时要按同样的路径带身份（双向 TLS 的两侧不能一个带一个不带）
        std::string m_clientCertificateFile{};
        std::string m_clientPrivateKeyFile{};
        /// HTTP/3 出站开关，默认关（语义与边界见 setHttp3Enabled）
        bool m_isHttp3Enabled{false};
        /// 探败过 h3 的端点：一个端点只探一次，此后这条客户端对它直接走 TCP
        std::set<HttpOutboundEndpointKey> m_http3RejectedEndpoints;
        /// 可选的 Cookie 存储罐：为空即完全不管 Cookie（不额外拷一份请求，也不改动任何头部）
        std::shared_ptr<HttpCookieJar> m_cookieJar;
        /// 出站熔断器。构造时即装一个默认配置的：塌掉的上游被反复重试是引擎侧的缺陷，
        /// 不该让每个使用方自己想起来开
        std::shared_ptr<OutboundCircuitBreaker> m_circuitBreaker;
    };
} // namespace AsynGyanis::Net
