/**
 * @file HttpStreamBody.h
 * @brief 一条流上的接收正文缓冲：作为 HttpBodySource，把 DATA 片段按到达批次交给处理器
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
     * @brief 一条流上的请求正文缓冲（HttpBodySource 在 h2/h3 上的共用实现）
     *
     * @details 会话把承载层交出的 DATA 片段追加进来，处理器经 HttpRequestBody 按批次取走，h1 那套「拉一次
     *          读一次」的语义在 h2/h3 上原样成立。背压落在「字节被消费」才经消费回调归还接收窗口：处理器
     *          不拉，窗口就不还，对端发到窗口用尽即停。
     * @note 两种承载的报量口径不同，由会话负责按各自规矩给出：h2 的流控把帧头与 padding 都算进去，
     *       因此报的是帧负载原长；h3 的流控按 STREAM 帧负载计，报的就是负载长度。
     * @note 推式承载（h3）的处理器停在 `readNext()` 等下一批时，会话没有「再读一批」这种动作可做，
     *       只能等承载把字节送上来，因此本类提供 `setBodyArrivedHandler()`：每次有新正文、收尾或
     *       断开都通知一次，会话据此唤醒等待者。拉式承载（h1/h2）不需要它。
     *
     * @warning bufferedBodyView() 的视图只在「下一次 discardBufferedBody() 之前」有效：
     *          本类不保证视图在 append() 之后仍然指向同一块内存，处理器必须在同一段里用完。
     * @note 未交付的字节数天然有上界（本流的接收窗口），因此流式路径不必再单独计入全局在途
     *       正文预算：窗口就是那道闸（非流式路径会把整份正文缓冲在请求对象里，才需要预算兜底）。
     */
    class HttpStreamBody final : public HttpBodySource
    {
    public:
        /// 消费回调：参数是本次被消费掉的**流控字节数**，会话据此归还接收窗口
        using ConsumeHandler = std::function<void(std::size_t)>;

        /// 到达通知：有新正文到达、收尾或断开时各调一次（供推式承载唤醒等待者）
        using BodyArrivedHandler = std::function<void()>;

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
         * @brief 绑定「有新正文到达」的通知（推式承载用它唤醒等正文的处理器）
         * @param bodyArrivedHandler 通知；传空表示不通知
         */
        void setBodyArrivedHandler(BodyArrivedHandler bodyArrivedHandler);

        /**
         * @brief 追加一段刚到达的正文
         * @param data 应用数据（帧层已剥掉 padding）
         * @param flowControlByteCount 该 DATA 帧占用的流控字节数：按承载的规矩给（见类注释）
         * @param endStream 对端在这一片之后收尾（正文到此为止）
         */
        void append(std::string_view data, std::size_t flowControlByteCount, bool endStream);

        /**
         * @brief 标记本流已不可继续（对端重置、连接失败）：处理器随即看到流终止
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

        /// 对端是否已收尾（正文收齐）
        [[nodiscard]] bool isComplete() const noexcept override;

        /// 本流是否已不可继续
        [[nodiscard]] bool isBroken() override;

        /// 正文不经请求对象中转，收齐后没有「残余」可交：恒为空视图
        [[nodiscard]] std::string_view completedBody() override;

    private:
        std::string        m_bytes;                          ///< 尚未交付的正文
        std::size_t        m_pendingFlowControlByteCount{0}; ///< 与 m_bytes 对应的流控字节数
        std::size_t        m_totalReceivedByteCount{0};      ///< 本流累计收到的正文字节数
        ConsumeHandler     m_consumeHandler;                 ///< 消费回调（会话借此归还接收窗口）
        BodyArrivedHandler m_bodyArrivedHandler;             ///< 到达通知（推式承载借此唤醒等待者）
        bool               m_isPeerFinished{false};          ///< 对端已收尾
        bool               m_isBroken{false};                ///< 流已不可继续
        bool               m_isBodyTooLarge{false};          ///< 正文总量越过会话上限
    };
} // namespace AsynGyanis::Net
