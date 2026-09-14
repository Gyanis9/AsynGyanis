/**
 * @file Http2StreamBody.h
 * @brief 一条 HTTP/2 流上的接收正文缓冲：作为 HttpBodySource，把 DATA 片段按到达批次交给处理器
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Net/Http/HttpBodySource.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    /**
     * @brief HTTP/2 上某条流的请求正文缓冲（HttpBodySource 的 h2 实现）
     *
     * @details 会话把连接层交出的 DATA 片段追加进来，处理器经 HttpRequestBody 按批次取走，
     *          于是 h1 那套「拉一次读一次」的语义在 h2 上原样成立。与 h1 的关键差别是背压的
     *          落地方式：h2 有自己的流控窗口，本类因此在**字节被消费**时才通过消费回调让会话
     *          归还接收窗口——处理器不拉，窗口就不还，对端发到窗口用尽即停。这是真正的背压，
     *          不是「先全收下来再慢慢处理」。
     *
     * @warning bufferedBodyView() 的视图只在「下一次 discardBufferedBody() 之前」有效：
     *          本类不保证视图在 append() 之后仍然指向同一块内存，处理器必须在同一段里用完。
     * @note 未交付的字节数天然有上界（本流的接收窗口），因此流式路径不必再单独计入全局在途
     *       正文预算：窗口就是那道闸（非流式路径会把整份正文缓冲在请求对象里，才需要预算兜底）。
     */
    class Http2StreamBody final : public HttpBodySource
    {
    public:
        /// 消费回调：参数是本次被消费掉的**流控字节数**（含 padding），会话据此归还接收窗口
        using ConsumeHandler = std::function<void(std::size_t)>;

        /**
         * @brief 为一条新流开始缓冲：清空内容与全部标志，并绑定消费回调
         * @param consumeHandler 消费回调；传空表示归还窗口这件事不由本类负责
         */
        void reset(ConsumeHandler consumeHandler);

        /**
         * @brief 只绑定消费回调，不动已缓冲的内容
         * @details 会话在开始服务这条流时才拿得到流号（回调要按流号还窗口），而正文可能早在那之前
         *          就已到达，因此不能顺手清空——清空用 reset()
         * @param consumeHandler 消费回调
         */
        void setConsumeHandler(ConsumeHandler consumeHandler);

        /**
         * @brief 追加一段刚到达的正文
         * @param data 应用数据（帧层已剥掉 padding）
         * @param flowControlByteCount 该 DATA 帧占用的流控字节数：帧负载原长（含 padding）
         * @param endStream 对端在这一片之后收尾（正文到此为止）
         */
        void append(std::string_view data, std::size_t flowControlByteCount, bool endStream);

        /**
         * @brief 标记本流已不可继续（对端 RST_STREAM、连接失败）：处理器随即看到流终止
         */
        void markBroken() noexcept;

        /**
         * @brief 标记正文总量已越过会话的上限：既终止流，也让会话据此回 413
         */
        void markBodyTooLarge() noexcept;

        /**
         * @brief 把还挂着的正文按已消费处理（业务不再读时由会话收口，顺带归还窗口）
         */
        void consumePending() noexcept;

        /**
         * @brief 尚未交付的字节数（背压判据：还挂着多少没被处理）
         * @return std::size_t 字节数
         */
        [[nodiscard]] std::size_t pendingByteCount() const noexcept;

        /**
         * @brief 本流累计收到的正文字节数（判会话的体量上限用，含已交付的部分）
         * @return std::size_t 字节数
         */
        [[nodiscard]] std::size_t totalReceivedByteCount() const noexcept;

        /**
         * @brief 正文是否已越过会话上限
         * @return true 表示会话已按体量越界收口这条流
         */
        [[nodiscard]] bool isBodyTooLarge() const noexcept;

        // ---- HttpBodySource ----
        /// 丢掉已交付的字节，并按量归还接收窗口
        void discardBufferedBody() noexcept override;

        /// 当前尚未交付的正文视图
        [[nodiscard]] std::string_view bufferedBodyView() const noexcept override;

        /// 对端是否已 END_STREAM（正文收齐）
        [[nodiscard]] bool isComplete() const noexcept override;

        /// 本流是否已不可继续
        [[nodiscard]] bool isBroken() override;

        /// h2 的正文不经请求对象中转，收齐后没有「残余」可交：恒为空视图
        [[nodiscard]] std::string_view completedBody() override;

    private:
        std::string    m_bytes;                          ///< 尚未交付的正文
        std::size_t    m_pendingFlowControlByteCount{0}; ///< 与 m_bytes 对应的流控字节数（含 padding）
        std::size_t    m_totalReceivedByteCount{0};      ///< 本流累计收到的正文字节数
        ConsumeHandler m_consumeHandler;                 ///< 消费回调（会话借此归还接收窗口）
        bool           m_isPeerFinished{false};          ///< 对端已 END_STREAM
        bool           m_isBroken{false};                ///< 流已不可继续
        bool           m_isBodyTooLarge{false};          ///< 正文总量越过会话上限
    };
} // namespace AsynGyanis::Net
