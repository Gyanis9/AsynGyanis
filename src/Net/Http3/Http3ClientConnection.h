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

#include "Core/Coroutine/Task.h"
#include "Net/Http3/Http3Connection.h"
#include "Net/Quic/QuicClientConnection.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
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
    struct Http3ClientResponse
    {
        int                                              statusCode{0};  ///< :status 的值；0 表示没拿到响应
        std::vector<std::pair<std::string, std::string>> headers;        ///< 除伪头之外的响应字段，按收到的顺序留着（含尾段字段）
        std::string                                      body{};         ///< 正文（DATA 帧拼接，额度已按消耗归还）
        std::string                                      errorMessage{}; ///< 失败时的中文原因；为空表示这条响应是正常收齐的
        /// 这条流上有没有收到过对端的任何字节。复用连接时靠它区分「对端在我们手里把连接收了」（可以重来
        /// 一次）与「响应本身出问题了」（重发会把非幂等请求做两遍）——与 h1/h2 侧同一位判据
        bool isAnyByteReceived{false};
        /// 这条流上有没有把字节写上过通路（只在写成功之后置位）
        bool isAnyByteSent{false};

        /// 是否成功收齐（拿到状态码且没有被对端或本端中止）
        [[nodiscard]] bool isOk() const noexcept
        {
            return statusCode != 0 && errorMessage.empty();
        }
    };

    /**
     * @brief HTTP/3 的客户端连接
     */
    class Http3ClientConnection
    {
    public:
        /**
         * @brief 本端能力与额度
         */
        struct Config
        {
            std::size_t maximumFieldSectionSizeByteCount{64U * 1024U}; ///< 本端愿收的最大头段字节数（RFC 9114 §4.2.2）
            std::size_t maximumOpenedStreamCount{1024U};               ///< 一条连接上最多开多少条请求流（流号到顶就要换代）
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
         * @param scheme 目标 URI 的协议名，写进 :scheme（h3 里恒为 "https"）
         * @param authority 目标主机[:端口]，写进 :authority
         * @param method 请求方法，写进 :method
         * @param path 请求路径（含查询串），写进 :path
         * @param extraHeaders 附加字段，按给出的顺序排在四个伪头之后
         * @param body 请求正文；为空时头段直接收尾这条流
         * @param waitTimeout 本次请求的整体时限（写出、等响应头、收完正文三段之和）
         * @return Http3ClientResponse 响应；失败时 errorMessage 给出断在哪一段
         * @warning 时限到点是**收掉整条连接**而不是只弃这条流：本层不替调用方揣测「同一条连接上别的
         *          请求还要不要」。因此复用一条连接时，超时的那一次会连带让其它在途请求拿不到答案，
         *          调用方按「对端在我们手里把连接收了」那一支重来即可。
         */
        [[nodiscard]] Core::Task<Http3ClientResponse> request(std::string_view scheme, std::string_view authority, std::string_view method, std::string_view path,
                                                              const std::vector<std::pair<std::string, std::string>> &extraHeaders, std::string_view body,
                                                              std::chrono::milliseconds waitTimeout);

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

        /// 这条连接是否还能提请求
        [[nodiscard]] bool isHealthy() const noexcept;

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
        };

        /// h3 那组按流的回调 → 本类的响应侧说法
        void noteHeaderField(std::int64_t streamId, std::string_view name, std::string_view value);
        void noteBodyBytes(std::int64_t streamId, std::span<const std::uint8_t> bytes);
        void noteMessageEnded(std::int64_t streamId);
        void noteStreamFailed(std::int64_t streamId, std::string_view reason);

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
} // namespace AsynGyanis::Net
