/**
 * @file Http3ClientConnection.h
 * @brief HTTP/3 的出站一侧：在一条已握手的 QUIC 出站连接上提请求、收响应
 * @author Gyanis
 * @date 2026-09-26
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 与 HTTP/2 侧的 `Http2ClientConnection` 位置对应。协议本身（帧布局、QPACK、控制流与
 *          SETTINGS）复用 `Http3Connection` 的客户端角色，本类只做三件事：把「一问一答」这件事
 *          排成一条本端双向流、把 h3 那组按流回调翻成响应侧的说法、以及在等响应时推动这条 QUIC
 *          连接收发。为什么不再往下抽一层：h3 的回调集合是「流上的一段消息」这种形状，两型各自
 *          的解释（服务端解释成请求、客户端解释成响应）本来就是各自的账本，抽出来只会得到一个
 *          两边都要往上补类型的中间人。
 *
 * @note 本类不自带后台协程：等响应时由 `request()` 自己一圈圈推（送已排好的字节 → 收一条报文）。
 *       几条请求并发时每条都在自己的 `request()` 里推，谁先收齐谁先返回。
 * @warning 只能在所属事件循环线程上用（继承 `QuicClientConnection` 的同一份线程契约）。
 * @see Http3Connection、QuicClientConnection
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/Coroutine/Task.h"
#include "Net/Http3/Http3Connection.h"
#include "Net/Quic/QuicClientConnection.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief 一次 HTTP/3 往来的结论
     * @details 字段形状与 `Http2ClientResponse` 逐字对齐，为的是调用方（出站池与 `HttpClient`）
     *          在两协议之间不用写两套判断。两份结构体没有合成一份：那份是 Http2 模块的公开类型，
     *          要合得先决定把它上收到哪一层——那是另一件事的范围。
     */
    struct ASYN_NET_API Http3ClientResponse
    {
        int                                              statusCode{0}; ///< :status 的值；0 表示没拿到响应
        std::vector<std::pair<std::string, std::string>> headers;       ///< 除伪头之外的响应头部字段，按收到的顺序留着
        /// 正文之后那个尾段的字段（RFC 9114 §4.3），按到达顺序留着。与 headers 分开：过去「含尾段字段」
        /// 混在同一张表里，调用方读不出哪一条是收完正文才知道的结果
        std::vector<std::pair<std::string, std::string>> trailers;
        std::string                                      body{};         ///< 正文（DATA 帧拼接，额度已按消耗归还）
        std::string                                      errorMessage{}; ///< 失败时的中文原因；为空表示这条响应是正常收齐的
        /// 这条流上有没有收到过对端的任何字节。复用连接时靠它区分「对端在我们手里把连接收了」（可以重来
        /// 一次）与「响应本身出问题了」（重发会把非幂等请求做两遍）——与 h1/h2 侧同一位判据
        bool isAnyByteReceived{false};
        /// 这条流上有没有把字节写上过通路（只在写成功之后置位）
        bool isAnyByteSent{false};
        /// 对端有没有**保证**这条请求没被处理过：RFC 9114 §5.2 的 H3_REQUEST_REJECTED（「The client can
        /// treat requests rejected by the server as though they had never been sent at all, thereby allowing
        /// them to be retried later」），以及 GOAWAY 通告值及以上的那些流（§7：「those requests will not be
        /// processed. Clients can safely retry unprocessed requests on a different HTTP connection」）。
        /// 与 h2 侧同名那一位同解：为真时重发对非幂等方法也安全
        bool isGuaranteedUnprocessed{false};

        /// 是否成功收齐（拿到状态码且没有被对端或本端中止）
        [[nodiscard]] bool isOk() const noexcept
        {
            return statusCode != 0 && errorMessage.empty();
        }
    };

    /**
     * @brief 一条 h3 响应正文的接收口，按到达批次交出这条流上的正文
     * @details 读法与 h2、HTTP/1.1 那两侧一致，只是头部落在 `Http3ClientResponse` 里。收口时本端
     *          RESET_STREAM + STOP_SENDING 结掉这一条流，连接留给别的请求用。
     * @details 挂了这个口，`head.body` 恒为空（字节都在批次里），且接收额度按交付进度归还——一批
     *          没交完就不抬 MAX_STREAM_DATA，本端缓冲的上界因此是一档接收窗口而不是正文总长。
     * @param head 这条流当前的响应记录（状态码与头部可信，`body` 恒为空；只在本次调用内有效）
     * @param batch 本批正文（只在本次调用内有效；`isLastBatch` 为真时可为空，表示零长收尾）
     * @param isLastBatch 是否最后一批：对端在这条流上收尾，或本端按上限判死
     * @return true 还要下一批；false 就此收口（本端结掉这条流，连接仍可用）
     */
    using Http3ResponseBodyReceiver = std::function<Core::Task<bool>(const Http3ClientResponse &head, std::string_view batch, bool isLastBatch)>;

    /**
     * @brief HTTP/3 的客户端连接
     */
    class ASYN_NET_API Http3ClientConnection
    {
    public:
        /**
         * @brief 本端能力与额度
         */
        /// 一条响应正文的默认字节上限：与 h1 解析器、h2 出站侧同档（三条通路共用一份胃口，
        /// 由 TestHttpResponseParser 里那条「默认档不许分叉」的用例盯着）
        static constexpr std::size_t kDefaultMaximumResponseBodyBytes = 8ull * 1024 * 1024;

        struct Config
        {
            std::size_t maximumFieldSectionSizeByteCount{64U * 1024U}; ///< 本端愿收的最大头段字节数（RFC 9114 §4.2.2）
            std::size_t maximumOpenedStreamCount{1024U};               ///< 一条连接上最多开多少条请求流（流号到顶就要换代）
            /// 一条响应正文的字节上限；0 表示不限。没有这道闸就是让对端决定本进程分配多少内存
            /// （h3 的正文长度由对端发多少 DATA 决定，声明了的 content-length 也防不住撒谎的对端）
            std::size_t maximumResponseBodyBytes{kDefaultMaximumResponseBodyBytes};
        };

        /**
         * @brief 建出站 h3 层，本端能力取 `Config` 的默认值
         * @param connection 已握完手的出站 QUIC 连接（非拥有；生命周期须覆盖本对象）
         */
        Http3ClientConnection(QuicClientConnection &connection) : Http3ClientConnection(connection, Config{})
        {
        }

        /**
         * @brief 同上，区别是本端能力由调用方给定
         * @param connection 已握完手的出站 QUIC 连接（非拥有；生命周期须覆盖本对象）
         * @param config 本端能力。默认值取不到这里来：`Config` 的成员初值属于本类的
         *        complete-class context，写成默认参数在 GCC 下非法（[class.mem]）
         */
        Http3ClientConnection(QuicClientConnection &connection, Config config);

        Http3ClientConnection(const Http3ClientConnection &) = delete;

        Http3ClientConnection &operator=(const Http3ClientConnection &) = delete;

        /**
         * @brief 开出三条本端单向流并把 SETTINGS 送上线
         * @return true 已可用（控制流与两条 QPACK 流都开出来了，SETTINGS 已交给传输层）
         * @return false 开不出来（连接未就绪、单向流额度为 0，或底层已收口）
         */
        [[nodiscard]] Core::Task<bool> start();

        /**
         * @brief 提一条请求并等它收齐；同一条连接上可以并发提多条（各占一条流）
         * @details 挂上 `responseReceiver` 就是逐批交付：这一支不再把正文攒进返回值的 body，
         *          而是每交完一批才归还那一档接收额度。
         * @param scheme 目标 URI 的协议名，写进 :scheme（h3 里恒为 "https"）
         * @param authority 目标主机[:端口]，写进 :authority
         * @param method 请求方法，写进 :method
         * @param path 请求路径（含查询串），写进 :path
         * @param extraHeaders 附加字段，按给出的顺序排在四个伪头之后
         * @param body 请求正文；为空时头段直接收尾这条流
         * @param waitTimeout 本次请求的整体时限（写出、等响应头、收完正文三段之和）
         * @param responseReceiver 响应正文的接收口；留空即整份攒进返回值的 body
         * @return Http3ClientResponse 响应；失败时 errorMessage 给出断在哪一段
         * @warning 时限到点是**收掉整条连接**而不是只弃这条流：本层不替调用方揣测「同一条连接上别的
         *          请求还要不要」。因此复用一条连接时，超时的那一次会连带让其它在途请求拿不到答案，
         *          调用方按「对端在我们手里把连接收了」那一支重来即可。
         */
        [[nodiscard]] Core::Task<Http3ClientResponse> request(std::string_view scheme, std::string_view authority, std::string_view method, std::string_view path,
                                                              const std::vector<std::pair<std::string, std::string>> &extraHeaders, std::string_view body,
                                                              std::chrono::milliseconds waitTimeout, const Http3ResponseBodyReceiver &responseReceiver = {});

        /**
         * @brief 礼貌收尾：发一条 GOAWAY 再关掉底层连接
         * @details 顺序与 h2 侧同理：GOAWAY 要让对端看见才有意义，连接一关就什么都发不出去。
         */
        Core::Task<void> shutdown();

        /**
         * @brief 直接关掉底层连接（不发 GOAWAY）
         * @note 幂等；之后 isHealthy() 为 false
         */
        void close() noexcept;

        /// 这条连接是否还能提请求：没被收口、底层 QUIC 还在、流号仍有余量
        /// （客户端流号严格递增、到顶就没有合法的新号可提，RFC 9000 §2.1），且对端没发过 GOAWAY
        /// （§5.2：「Endpoints MUST NOT initiate new requests … after receipt of a GOAWAY frame from the peer」）
        [[nodiscard]] bool isHealthy() const noexcept;

        /**
         * @brief 对端这条流的收法算不算「保证没处理过」，即可以当没发过重来一次
         * @details RFC 9114 §5.2：H3_REQUEST_REJECTED 的意思是「A server rejected a request without performing
         *          any application processing」，客户端「can treat requests rejected by the server as though
         *          they had never been sent at all」；同节还规定服务端 MUST NOT 对已部分或全部处理过的请求
         *          用这个码。本端再加一道自己的防御：**收到过任何响应字节就不认**——对端答过话又说没处理，
         *          那是它违规，不能拿它的话把非幂等请求做两遍。别的码（H3_REQUEST_CANCELLED、
         *          H3_INTERNAL_ERROR 等）都不带这个保证，按「可能已经执行过」处置
         * @param errorCode 对端 RESET_STREAM 里带的 h3 错误码
         * @param isAnyByteReceived 这条流上本端有没有收到过响应字节
         * @return true 表示重来一次是安全的（连非幂等方法也算）
         */
        [[nodiscard]] static bool isUnprocessedRejection(Http3ErrorCode errorCode, bool isAnyByteReceived) noexcept;

        /// 在途（已提出、还没收齐）的请求流条数：连接池据此判断这条连接是不是正被人用着
        [[nodiscard]] std::size_t inFlightStreamCount() const noexcept;

        /// 本端已开过的请求流条数，用来判「流号要用尽了，该换一条连接」
        [[nodiscard]] std::size_t openedStreamCount() const noexcept
        {
            return m_openedStreamCount;
        }

    private:
        /// 一条在途请求的账：响应本身，加上「收齐没有」「断在哪一段」
        struct PendingExchange
        {
            Http3ClientResponse response{};        ///< 逐段填起来的结论
            bool                isComplete{false}; ///< 收到收尾（FIN/流关闭），或已被判死

            // 响应正文的接收口；空表示整份攒进 response.body。挂了就多一条规矩：接收额度按交付
            // 进度归还，本端缓冲的上界因此是一档窗口而不是正文总长
            Http3ResponseBodyReceiver responseReceiver{};
            std::string               undeliveredBodyBytes{};       ///< 到了货但还没交给接收口的那段正文
            std::size_t               receivedBodyByteCount{0};     ///< 这条流上累计收到的正文字节（上限按它判，不按缓冲）
            bool                      isFinalBatchDelivered{false}; ///< 收尾那一批（含零长收尾）已经交出去了
        };

        /// h3 那组按流的回调 → 本类的响应侧说法
        /// 收下解出的一个响应字段。伪头 :status 折成状态码后不留字段；其余按 isTrailers 分档——
        /// 正文之后的尾段落进 response.trailers，与响应头部分开留，调用方才读得出「这是收完正文
        /// 才知道的结果」
        void noteHeaderField(std::int64_t streamId, std::string_view name, std::string_view value, bool isTrailers);
        void noteBodyBytes(std::int64_t streamId, std::span<const std::uint8_t> bytes);
        void noteMessageEnded(std::int64_t streamId);
        void noteStreamFailed(std::int64_t streamId, std::string_view reason);

        /**
         * @brief 取这条流在途的账；已经不认的流交出空条目，**不新建记录**
         * @details 请求协程收口时会把这条流的账摘掉，而对端在途的字节还能后到（本端刚结掉一条流，
         *          STOP_SENDING 至少还要一个来回才到）。那时按 `m_pendingStreams[id]` 取就会凭空
         *          立一条谁也不会再摘掉的记录——在途数从此再也回不到 0，链路看着一直被人用着。
         * @param streamId 来字节的流
         * @return PendingExchange* 这条流的账；本端已经不认了则为空
         */
        [[nodiscard]] PendingExchange *liveExchange(std::int64_t streamId) noexcept;

        /// 把这条流上到了货的正文交一批给接收口；返回 false 表示接收口收口了（本端已结掉这条流）
        Core::Task<bool> deliverReceivedBody(PendingExchange &exchange, std::int64_t streamId);

        /// 把这条连接上所有在途请求按同一原因判死（连接被收掉时用）
        void failAllPending(std::string_view reason);

        /// 取（必要时新建）某条流的在途账
        PendingExchange &exchangeFor(std::int64_t streamId);

        QuicClientConnection                   &m_connection;            ///< 底层出站 QUIC 连接（非拥有）
        Config                                  m_config;                ///< 本端能力
        std::unique_ptr<Http3Connection>        m_protocol{};            ///< h3 协议层（客户端角色）
        std::map<std::int64_t, PendingExchange> m_pendingStreams{};      ///< 在途的请求流
        std::size_t                             m_openedStreamCount{0U}; ///< 本端已开过的请求流条数
        bool                                    m_isHealthy{false};      ///< 是否可继续提请求
    };

    /**
     * @brief 一条出站 h3 链路的持有对：QUIC 连接与 h3 会话同生同灭
     *
     * @details 存在的理由是**共同持有**。`Http3ClientConnection` 只借用它的 QUIC 连接（不拥有），
     *          而出站池要把一条可复用的 h3 连接交给多个在途请求共同持有——把两份所有权分开交出去，
     *          就会出现「会话还在人手里、底下的 QUIC 连接先被释放」那种踩空。成对构造、成对释放，
     *          拿到 shared_ptr 的人就同时保住两层。
     *
     * @note 成员声明顺序承担一件不看代码想不到的事：会话必须比它的 QUIC 连接**先**销毁（它持有那个
     *       引用），而成员是按声明逆序销毁的，所以会话排在后面。改动这两个成员的次序是会崩的。
     * @warning 只能在所属事件循环线程上用（与 `Http3ClientConnection` 同一份线程契约）。
     */
    class ASYN_NET_API Http3OutboundLink
    {
    public:
        /**
         * @brief 建链路：造出 QUIC 连接并把 h3 会话挂在它上面（不碰网络）
         * @param loop 所属事件循环
         * @param configuration 出站 QUIC 配置（主机名、ALPN、信任库、时限）
         * @param http3Configuration h3 本端能力；默认取 `Http3ClientConnection::Config` 的默认值
         */
        Http3OutboundLink(Core::EventLoop &loop, QuicClientConnection::Configuration configuration, Http3ClientConnection::Config http3Configuration = {});

        Http3OutboundLink(const Http3OutboundLink &)            = delete;
        Http3OutboundLink &operator=(const Http3OutboundLink &) = delete;
        Http3OutboundLink(Http3OutboundLink &&)                 = delete;
        Http3OutboundLink &operator=(Http3OutboundLink &&)      = delete;
        ~Http3OutboundLink()                                    = default;

        /**
         * @brief 握一条 QUIC 连接并把 h3 层起起来（三条本端单向流 + SETTINGS）
         * @param serverAddress 目标地址
         * @return true 这条链路已经可以提请求
         * @return false 握手或 h3 起步没成；调用方按「换一条通路」处置（本对象此后不会再变健康）
         */
        [[nodiscard]] Core::Task<bool> connect(const Core::InetAddress &serverAddress);

        /// h3 会话本体（提请求、礼貌收尾都走它）
        [[nodiscard]] Http3ClientConnection &http3() noexcept
        {
            return m_http3;
        }

        /// 这条链路还能不能提请求：会话健康且 QUIC 连接没被收掉
        [[nodiscard]] bool isHealthy() const noexcept;

        /// 在途（已提出、还没收齐）的请求流条数：池判断「这条能不能立刻收掉」与观测复用度都读它
        [[nodiscard]] std::size_t inFlightStreamCount() const noexcept
        {
            return m_http3.inFlightStreamCount();
        }

        /// 本端已开过的请求流条数：流号要用尽时池据此换一条新链路
        [[nodiscard]] std::size_t openedStreamCount() const noexcept
        {
            return m_http3.openedStreamCount();
        }

    private:
        std::unique_ptr<QuicClientConnection> m_quic{}; ///< 先建、后销毁：h3 会话引用它
        Http3ClientConnection                 m_http3;  ///< 后建、先销毁（见类注释里的成员次序）
    };

} // namespace AsynGyanis::Net
