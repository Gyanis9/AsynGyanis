/**
 * @file HttpOutboundConnectionPool.h
 * @brief 出站侧的 keep-alive 连接与按主机归组的连接池：一次请求一条连接的开销只付一次
 * @author Gyanis
 * @date 2026-09-25
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once

#include "Core/Coroutine/Task.h"
#include "Net/Http/Client/HttpOutboundEstablishment.h"
#include "Net/Http/Client/HttpResponseParser.h"
#include "Net/Tcp/TcpStream.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
    class TlsSocket;
} // namespace AsynGyanis::Core

namespace AsynGyanis::Net
{
    class Http2ClientConnection;

    /**
     * @brief 一条出站连接的可复用身份：主机、端口与是否 TLS
     * @details 三者缺一都不能复用：换了主机要重做 DNS 与 TLS（SNI 与证书校验的主机名也跟着换），
     *          换了 TLS 与否更是把明文写进对端的 TLS 会话。池按这个键归组。
     */
    struct HttpOutboundEndpointKey
    {
        std::string host;              ///< 主机名或 IP 字面量，按 URL 给出的原文存（不做大小写归一）
        std::uint16_t port{0};         ///< 端口
        bool isTls{false};             ///< 是否为 TLS 连接

        /**
         * @brief 两个键是否指向同一条可复用的通路
         * @param other 另一个键
         * @return true 主机、端口与 TLS 三项都相同
         */
        [[nodiscard]] bool operator==(const HttpOutboundEndpointKey &other) const noexcept;

        /**
         * @brief 给池里的分组定序（std::map 要的是一个「严格弱序」，不是某种业务次序）
         * @details 按 TLS 位、端口、主机的顺序比：先按位比能把明文与 TLS 两组彻底分开，
         *          端口是定长数值比主机名便宜，所以放在主机之前
         * @param other 另一个键
         * @return true 本键排在 other 之前
         */
        [[nodiscard]] bool operator<(const HttpOutboundEndpointKey &other) const noexcept;
    };

    /**
     * @brief 池里的一条已建立连接：明文走 TcpStream，TLS 走 TlsSocket
     * @details 解析器与套接字同生共死是 keep-alive 的关键一环：上一条响应收齐时，读进来的字节往往
     *          已经越过了响应边界，那部分只属于**下一条**响应。换掉解析器就等于把它丢掉。
     * @warning 只能在自己的事件循环上收发（协程挂起期间被别的线程驱动会踩坏套接字状态），因此本对象
     *          连同所在池都不跨线程共享。
     */
    class HttpOutboundConnection
    {
    public:
        /**
         * @brief 接管一条明文连接
         * @param endpointKey 这条连接的可复用身份
         * @param stream 已连上的明文流
         * @return std::unique_ptr<HttpOutboundConnection> 池化连接
         */
        [[nodiscard]] static std::unique_ptr<HttpOutboundConnection>
                forPlain(HttpOutboundEndpointKey endpointKey, TcpStream stream);

        /**
         * @brief 接管一条已完成 TLS 握手的连接
         * @param endpointKey 这条连接的可复用身份
         * @param socket 已握手的 TLS 套接字（本对象接管其所有权）
         * @return std::unique_ptr<HttpOutboundConnection> 池化连接
         */
        [[nodiscard]] static std::unique_ptr<HttpOutboundConnection>
                forSecure(HttpOutboundEndpointKey endpointKey, std::unique_ptr<Core::TlsSocket> socket);

        HttpOutboundConnection(const HttpOutboundConnection &) = delete;
        HttpOutboundConnection &operator=(const HttpOutboundConnection &) = delete;
        HttpOutboundConnection(HttpOutboundConnection &&) = delete;
        HttpOutboundConnection &operator=(HttpOutboundConnection &&) = delete;
        ~HttpOutboundConnection();

        /// 本连接的可复用身份
        [[nodiscard]] const HttpOutboundEndpointKey &endpointKey() const noexcept { return m_endpointKey; }

        /// 本连接用的解析器：调用方在喂字节之前标记 HEAD 应答，收完一条后不必复位（reset 由本类做）
        [[nodiscard]] HttpResponseParser &parser() noexcept { return m_parser; }

        /**
         * @brief 读一段字节
         * @param buffer 接收缓冲区首地址
         * @param length 缓冲区容量
         * @return Core::Task<ssize_t> 实际读到的字节数；0 表示对端正常收口，负数表示读错误
         */
        [[nodiscard]] Core::Task<ssize_t> receive(void *buffer, std::size_t length);

        /**
         * @brief 写完一段字节
         * @param data 待写数据
         * @return Core::Task<bool> 全部写出为 true；出错、对端关闭或本端已收口为 false
         */
        [[nodiscard]] Core::Task<bool> send(std::string_view data);

        /**
         * @brief 本连接是否还能收发（没被 close()、也没被超时看门狗掐断）
         * @return true 还开着
         */
        [[nodiscard]] bool isOpen() const noexcept;

        /**
         * @brief ALPN 协商出的协议名（"h2"、"http/1.1"）
         * @details 调用方据此决定按哪种协议说话：明文连接没有 ALPN，返回空串；TLS 连接上客户端没提
         *          ALPN、或对端不协商时也返回空串——那都意味着「按 HTTP/1.1 来」。
         * @return std::string 协商出的协议名；没协商出来时为空
         */
        [[nodiscard]] std::string selectedAlpnProtocol() const;

        /**
         * @brief 收口这条连接：明文侧关描述符，TLS 侧连 SSL 对象一起释放
         * @note 幂等；之后 isOpen() 为 false，任何收发都只会被拒绝
         */
        void close() noexcept;

        /**
         * @brief 为下一条响应复位解析器（上一条收齐后必须调，否则会拿旧状态去解新报文）
         */
        void prepareForNextRequest() noexcept;

    private:
        HttpOutboundConnection(HttpOutboundEndpointKey endpointKey, std::unique_ptr<TcpStream> plainSocket,
                               std::unique_ptr<Core::TlsSocket> tlsSocket);

        /// 两条通路各持其一，另一个恒为空——分发方式与 HTTP/2 会话里的传输选择一致（按 has_value 分支）
        std::unique_ptr<TcpStream> m_plainSocket;
        std::unique_ptr<Core::TlsSocket> m_tlsSocket;
        HttpOutboundEndpointKey m_endpointKey;
        HttpResponseParser m_parser;
        /// 一次收发失败或对端收口就置否：连接能不能复用只看这个字，不看描述符还在不在——
        /// 描述符健在而字节序已经乱了（读到半条响应、写到一半被拒）的连接绝不能再交给别人
        bool m_isOpen{true};
    };

    /**
     * @brief 出站 keep-alive 连接池：按主机+端口+TLS 归组，只存空闲连接
     * @details 只存空闲的这一半是有意的：在途连接由请求自己持有，池不知道它的字节走到哪了；让池同时
     *          管两种状态等于把「一条连接同时被两个请求写」的窗口开回来（HTTP/1.1 没有流的概念，响应
     *          严格按请求顺序回来，混写会让两条响应串到彼此身上）。
     */
    class HttpOutboundConnectionPool
    {
    public:
        /// 空闲连接的默认保留时长：对端大多在 30–60 秒之间收掉闲置连接，留到这儿既省重连又不至于
        /// 拿到一条已被对端关掉的死连接
        static constexpr std::chrono::milliseconds kDefaultIdleTimeout{30000};

        /// 每个键默认留几条空闲连接：再多就是白占描述符，而一次突发通常也用不上更多
        static constexpr std::size_t kDefaultMaximumIdlePerEndpoint = 4;

        /**
         * @brief 一条响应正文的默认字节上限：与 `HttpResponseParser::kDefaultMaximumBodySize` 同档
         * @details 没有这道闸就是让对端决定本进程分配多少内存（chunked 与「读到连接关闭」两种定界
         *          下正文长度由对端说了算）。填 0 表示不限——要收大文件的使用方按这个开关放开。
         * @note HTTP/1.1 与 HTTP/2 两条通路共用同一个默认值，且它由一条用例盯着不许分叉
         *       （协商出哪条协议不该改变本端的胃口）
         */
        static constexpr std::size_t kDefaultMaximumResponseBodyBytes = 8ull * 1024 * 1024;

        /**
         * @brief 池的规模参数
         * @param idleTimeout 空闲多久就收口
         * @param maximumIdlePerEndpoint 每个键最多留几条
         * @param maximumResponseBodyBytes 一条响应正文的字节上限，0 表示不限
         */
        struct Config
        {
            std::chrono::milliseconds idleTimeout{kDefaultIdleTimeout};
            /// 每台主机留几条空闲连接；0 表示不池化（用完当场收口）。填 0 是合法的关闭开关，
            /// 不是配置错误：负面的做法是让它去把表里唯一那条挤掉，那等于留了一条
            std::size_t maximumIdlePerEndpoint{kDefaultMaximumIdlePerEndpoint};
            /// 本端愿意收多大的响应正文；越过就判这次请求失败，连接不留残货。放开到不限填 0
            std::size_t maximumResponseBodyBytes{kDefaultMaximumResponseBodyBytes};
        };

        /// 用默认参数建池
        HttpOutboundConnectionPool() noexcept = default;

        /**
         * @brief 指定参数建池
         * @param config 空闲时长与每键条数
         */
        explicit HttpOutboundConnectionPool(Config config) noexcept;

        HttpOutboundConnectionPool(const HttpOutboundConnectionPool &) = delete;
        HttpOutboundConnectionPool &operator=(const HttpOutboundConnectionPool &) = delete;

        /**
         * @brief 析构：把池里还留着的连接全部收口
         * @details 定义在 .cpp 里——h2 连接在本头只有前置声明，持有它的 unique_ptr 要看到完整类型
         *          才能生成销毁代码，把析构留在头里就等于让每个包含者都得包含 Http2 那层。
         */
        ~HttpOutboundConnectionPool();

        /**
         * @brief 本池生效的参数
         * @return const Config & 建池时给定的那份（含默认值），调用方据此配置自己新建的连接
         * @details 存在的理由是「响应正文上限」这件事由使用方定，而连接是 HttpClient 建的：池把这一
         *          份配置交出去，两条通路（HTTP/1.1 的解析器与 HTTP/2 的按流缓冲）才吃得到同一个数
         */
        [[nodiscard]] const Config &config() const noexcept { return m_config; }

        /**
         * @brief 取一条可复用的空闲连接，并顺手收掉过期与已被对端关掉的
         * @param endpointKey 目标身份
         * @return std::unique_ptr<HttpOutboundConnection> 取到则交出所有权；没有可复用的返回空
         */
        [[nodiscard]] std::unique_ptr<HttpOutboundConnection> acquire(const HttpOutboundEndpointKey &endpointKey);

        /**
         * @brief 用完还回池里；放不下或已经不能用了就直接收口
         * @param connection 请求结束后的连接（交出所有权；传空是本就决定丢弃这条，同样返回）
         */
        void release(std::unique_ptr<HttpOutboundConnection> connection);

        /**
         * @brief 取回某台主机上留着的那条 HTTP/2 连接
         * @details h2 的连接可以**同时**给好几个请求用（复用发生在流上），所以这里不交出所有权：
         *          取到的人手里只是一份共同持有的引用，取用次数不影响它在池里的位置。已断开或已被
         *          对端收尾的那条在这里当场作废并返回空——判死不留缓存，HPACK 动态表跟着连接一起作废。
         * @param endpointKey 目标身份
         * @return std::shared_ptr<Http2ClientConnection> 可用则给出一份引用；没有可复用的返回空
         */
        [[nodiscard]] std::shared_ptr<Http2ClientConnection> acquireHttp2(const HttpOutboundEndpointKey &endpointKey);

        /**
         * @brief 把一条刚建好的 h2 连接放进缓存；不可用的直接不收
         * @param endpointKey 这条连接的目标身份
         * @param connection 已完成前奏的连接（与调用方共同持有）
         */
        void adoptHttp2(const HttpOutboundEndpointKey &endpointKey, std::shared_ptr<Http2ClientConnection> connection);

        /// 池里留着的 h2 连接条数（测试与观测用；与下面那条 h1 的空闲数各量各的）
        [[nodiscard]] std::size_t idleHttp2ConnectionCount() const noexcept;

        /**
         * @brief 最忙的那条 h2 连接上同时在途的流数（测试与观测用）
         * @details 单看连接条数判不出复用：两条连接各服务一条请求，与一条连接同时服务两条，那个数都是 2。
         *          这里取的是各条连接的**最大值**，也就是本端实测到的复用度——一台主机只留一条连接，
         *          故「连接一条、复用度二」只能解释为两条请求共用了同一条连接。
         * @return std::size_t 各条留着的 h2 连接里在途流数最大的那个；池里没货返回 0
         */
        [[nodiscard]] std::size_t http2MaximumInFlightStreamCount() const noexcept;

        /// 当前空闲条数（测试与观测用）
        [[nodiscard]] std::size_t idleConnectionCount() const noexcept;

        /// 归还所有权：池里所有连接当场收口
        void closeAll() noexcept;

        /**
         * @brief 占下这个端点的「建连」资格：占到的人负责建，没占到的人等
         * @param endpointKey 目标身份
         * @return true 本次成为领导者：去建连，每条出口都**必须** settleEstablishment()
         * @return false 已有别的请求在建：改 co_await awaitEstablishment()，醒来再看池
         * @details 存在的理由：冷池上同时进来的请求原本各建一条连接，5 条并发就是 5 遍
         *          TCP+TLS+ALPN+h2 前奏（实测服务器侧同时在册 5 条），而 HTTP/2 的复用就发生在这条
         *          连接上。合并之后同批请求共用一次握手。
         * @see awaitEstablishment, settleEstablishment
         */
        bool tryBeginEstablishment(const HttpOutboundEndpointKey &endpointKey);

        /**
         * @brief 结算一个端点的建连：撤掉在途标记并唤醒等待者
         * @param endpointKey 目标身份
         * @details 不论建成还是失败都要调，且**不能靠作用域守卫**：本框架的协程帧不在 `co_return` 时
         *          销毁（FinalAwaiter 是 no-op），守卫要等调用方放手才触发，那时等待者已经白等一整段
         *          请求时间。所以领导者要在交出结论的当下显式结算。
         */
        void settleEstablishment(const HttpOutboundEndpointKey &endpointKey);

        /**
         * @brief 等某个端点的建连结算（协程用）
         * @param endpointKey 等哪个端点
         * @param loop 本协程所属的事件循环（唤醒投回这里）
         * @return HttpEstablishmentAwait 可直接 co_await；醒来后调用方要重新看一遍池
         * @note 等待本身不设时限：领导者那条建连被它自己的请求时限管着，它一结算这里就醒。
         *       醒来之后本端仍要自己算剩余预算——这一段等待可能已经吃掉了一部分。
         */
        [[nodiscard]] HttpEstablishmentAwait awaitEstablishment(const HttpOutboundEndpointKey &endpointKey,
                                                                Core::EventLoop &loop);

    private:
        /// 端点键在记账表里的文本形态（表按字符串分组，转换只在这一处）
        [[nodiscard]] static std::string establishmentKeyOf(const HttpOutboundEndpointKey &endpointKey);

        using Clock = std::chrono::steady_clock;

        /// 一条空闲连接连同它最后一次被使用的时间
        struct IdleEntry
        {
            std::unique_ptr<HttpOutboundConnection> connection;
            Clock::time_point idleSince;
        };

        /// 按键分组的空闲连接：键的顺序不稳定问题不成问题（这里只按组取用，不对外给出次序）
        std::map<HttpOutboundEndpointKey, std::vector<IdleEntry>> m_idleByEndpoint;

        /// 每台主机留一条 h2 连接，与调用方共同持有：它不像 h1 那样一次租给一个请求，也不按
        /// idleTimeout 收（对端还认它就一直用），要收的是「已经断了」这件事——只在取用时判
        std::map<HttpOutboundEndpointKey, std::shared_ptr<Http2ClientConnection>> m_http2ByEndpoint;

        /// 「同一端点同时只建一次连」的记账表： shared_ptr 是为了让挂着的等待者不必依赖池还活着
        std::shared_ptr<HttpEstablishmentTable> m_establishments{std::make_shared<HttpEstablishmentTable>()};

        Config m_config;
    };
} // namespace AsynGyanis::Net
