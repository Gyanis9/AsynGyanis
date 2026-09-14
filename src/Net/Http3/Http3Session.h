/**
 * @file Http3Session.h
 * @brief 一条 QUIC 连接上的 HTTP/3 会话：绑定控制流与 QPACK 流，把 h3 帧交给 nghttp3
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

/// nghttp3 的连接对象只以指针形式出现在本文件里，实现细节留在 .cpp——这样 Net 可以私有链接
/// nghttp3（消费方不必被迫去 find_package 它，导出包也少一处 find_dependency）
struct nghttp3_conn;

namespace AsynGyanis::Net
{
    /**
     * @brief 一条 QUIC 连接上的 HTTP/3 会话
     *
     * @details 与 HTTP/2 侧 `Http2Session` 的位置对应：都是「一条连接上的多路复用」，区别只是多路的
     *          载体从 TCP 帧换成了 QUIC 流。h3 这一层里，帧的组装与拆分、QPACK 的编解码都交给
     *          nghttp3，本类负责把流数据在 nghttp3 与传输层之间搬。
     *
     * @note 控制流与 QPACK 编解码流都是**本端发起的单向流**，必须在会话建立时就开出来交给 nghttp3
     *       绑定：少了它们 nghttp3 连 SETTINGS 都发不出去（RFC 9114 §6.2.1）。流号由传输层给
     *       （`StreamOpener`），本类不碰 ngtcp2。
     * @warning 线程契约与连接一致：本对象只在其所属事件循环线程上使用。
     */
    class Http3Session
    {
    public:
        /// 开一条本端发起的单向流并返回流号（由 QuicConnection 提供；失败返回 -1）
        using StreamOpener = std::function<std::int64_t()>;

        /// 待发流数据的出口（由 QuicConnection 提供，内部就是 queueStreamData）
        using StreamWriter = std::function<void(std::int64_t streamId, std::span<const std::uint8_t> data, bool endStream)>;

        /**
         * @brief 建立一个 HTTP/3 服务端会话
         * @param opener 单向流的开流口
         * @param writer 流数据出口
         * @note 构造里就把控制流与两条 QPACK 流绑上。开流失败只记日志并让会话保持不可用
         *       （`isUsable()` 为假），不抛异常：一条连接建不起 h3 不该把服务端拖垮
         */
        Http3Session(StreamOpener opener, StreamWriter writer);

        ~Http3Session();

        Http3Session(const Http3Session &) = delete;

        Http3Session &operator=(const Http3Session &) = delete;

        /**
         * @brief 会话是否可用（控制流与 QPACK 流都绑上了）
         * @return true 可用
         */
        [[nodiscard]] bool isUsable() const noexcept;

        /**
         * @brief 会话是否已作废
         * @return true 已作废（nghttp3 判定协议错误），此后唯一合法的动作是销毁
         */
        [[nodiscard]] bool isBroken() const noexcept;

        /**
         * @brief 把对端在一条流上送来的字节交给 HTTP/3 层
         * @param streamId 流号
         * @param data 本段字节
         * @param isEndStream 对端在这段之后收尾
         */
        void onStreamData(std::int64_t streamId, std::span<const std::uint8_t> data, bool isEndStream);

        /**
         * @brief 把 HTTP/3 层攒下的待发字节交给传输层
         */
        void flushPendingStreamData();

    private:
        /// 一次 flush 最多搬多少段：防止待发字节很多时在一条连接上转太久
        static constexpr std::size_t kMaximumWritesPerFlush = 64;

        /// 一次取待发数据最多用多少个分片描述
        static constexpr std::size_t kMaximumDataVectors = 16;

        /**
         * @brief 用 nghttp3 的错误码记日志并把会话作废
         * @param errorCode nghttp3 返回的负错误码
         * @param what 正在做的事（进日志）
         */
        void markBroken(int errorCode, const char *what);

        nghttp3_conn             *m_connection{nullptr}; ///< nghttp3 连接对象
        StreamWriter              m_writer;              ///< 流数据出口
        std::vector<std::uint8_t> m_pendingBytes;        ///< 一次 flush 用的连续缓冲（把分片拼在一起）
        bool                      m_isUsable{false};     ///< 三条单向流是否都绑上了
        bool                      m_isBroken{false};     ///< 是否已作废
    };
} // namespace AsynGyanis::Net
