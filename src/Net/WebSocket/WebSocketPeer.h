/**
 * @file WebSocketPeer.h
 * @brief WebSocket 对端对象：101 之后交给业务处理器的收发句柄（RFC 6455 §5）
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Coroutine/Task.h"
#include "Net/WebSocket/WebSocketFrame.h"

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    class HttpMetricsCollector;

    class WebSocketPeer;

    /**
     * @brief 升级成功后的业务处理器：独占这条连接，直到它返回或连接收口
     *
     * @details 处理器只在 WebSocketPeer 的 receive()/send*() 上挂起，读写与关闭握手都由会话阶段
     *          驱动，因此它不需要（也不应该）自己接触 socket。
     */
    using WebSocketHandler = std::function<Core::Task<>(WebSocketPeer &)>;

    /// 正常关闭状态码（RFC 6455 §7.4.1）
    inline constexpr std::uint16_t kWebSocketNormalClosureCode = 1000;

    /// 协议错误状态码：帧格式违规（掩码缺失、RSV 非 0、未定义操作码等）
    inline constexpr std::uint16_t kWebSocketProtocolErrorCode = 1002;

    /// 负载数据非法状态码：文本帧负载不是合法 UTF-8（RFC 6455 §7.4.1）
    inline constexpr std::uint16_t kWebSocketInvalidPayloadDataCode = 1007;

    /// 消息过大状态码：单帧或重组后的消息超出解码层上限
    inline constexpr std::uint16_t kWebSocketMessageTooBigCode = 1009;

    /**
     * @brief 一条完整的数据消息
     *
     * @details 分片消息已在解码层重组，因此一条消息恰好对应一次 receive() 返回；
     *          opCode 只可能是 Text 或 Binary，控制帧不会走到业务手里。
     */
    struct WebSocketMessage
    {
        WebSocketOpCode opCode{WebSocketOpCode::Text}; ///< 消息类型：Text 或 Binary
        std::string     payload;                       ///< 消息负载：Text 已校验为合法 UTF-8，Binary 为任意字节（两者都可含 NUL）
    };

    /**
     * @brief 会话侧把一段网络字节喂进解码器的结论
     */
    enum class WebSocketFeedStatus
    {
        Accepted,   ///< 本段字节已全部消费完（可能产出了消息）
        DecodeError ///< 对端违反 RFC 6455：解码器进入粘滞错误态，原因见 decodeErrorText()
    };

    /**
     * @brief 一条已升级连接的对端对象：业务的收发句柄，同时持有帧解码器与发送路径
     *
     * @details 会话在回完 101 之后构造本对象并交给业务处理器：解出的帧先在此排队、由 receive()
     *          取走，发出的帧经会话注入的回调直接写这条连接。分片重组、掩码与 RSV 校验都在解码层完成；
     *          文本帧负载的 UTF-8 校验（RFC 6455 §5.6）在此层完成，即「交给业务之前」这一步。
     *
     * @note 文本负载一旦判为非法就不再交付业务：feedBytes() 当场返回 DecodeError，由会话发出
     *       1007（invalid frame payload data）并收口，`WebSocketMessage` 因此不会带非法文本。
     *
     * @note 线程约束：全部方法都只在所属事件循环线程上调用，内部状态不加锁。
     * @warning 会话收尾（读到 EOF、解码失败、业务返回）会把本对象标记为关闭：此后 send*() 一律
     *          返回 false、receive() 一律返回空。**挂起在 receive() 上的业务协程不会被唤醒**，
     *          它的帧随会话一起销毁，因此处理器不能把「收到空结果」当作唯一的退出通知。
     */
    class WebSocketPeer
    {
    public:
        /**
         * @brief 帧发送回调：把一整帧已编码的字节写到这条连接
         *
         * @details 由会话在构造本对象时注入（装配风格与 HttpResponse::ChunkSender 一致）。false 表示
         *          连接已不可用：传输层失败（对端关闭或复位、描述符被关闭、等可写期间被关闭）由会话在
         *          回调内部折成 false 并记日志，不抛异常；回调是协程，写不下时会挂起等待可写。
         */
        using FrameSender = std::function<Core::Task<bool>(std::string_view)>;

        /**
         * @brief 构造对端对象并注入发送路径与统计采集端
         * @param frameSender 发送回调，由会话装配；本对象没有「未装配」状态，必须给出可用的回调
         * @param metrics 统计采集端；传空指针表示本对象不上报 WebSocket 各项计数。它由服务器持有、
         *        按 shared_ptr 共享，本对象只借用，因此比它活得久也不会写到已释放对象上
         */
        explicit WebSocketPeer(FrameSender frameSender, HttpMetricsCollector *metrics = nullptr);

        /**
         * @brief 析构函数：成员都是按值的标准容器，无额外资源需要回收。
         */
        ~WebSocketPeer() = default;

        // 禁拷贝与移动：对端对象与它所在会话的协程帧一对一，复制一份会让「消息该交给谁」分叉
        WebSocketPeer(const WebSocketPeer &) = delete;

        WebSocketPeer &operator=(const WebSocketPeer &) = delete;

        WebSocketPeer(WebSocketPeer &&) = delete;

        WebSocketPeer &operator=(WebSocketPeer &&) = delete;

        /**
         * @brief 收下一条数据消息
         *
         * @details 队列里已有消息时立即返回；否则挂起，直到会话交来一条消息或连接收口。
         *          期间收到的 Ping/Pong/Close 在此就地处理，不会作为消息交给调用方。
         * @return 消息；std::nullopt 表示对端关闭或连接不可用，调用方应停止收发并返回
         * @note 返回空之后再次调用会立即再次返回空，不会挂起
         */
        Core::Task<std::optional<WebSocketMessage>> receive();

        /**
         * @brief 发送一条文本消息
         * @param text 文本内容，按「指针 + 长度」取，UTF-8 合法性不在协议层校验
         * @return true 整帧已交给连接
         * @return false 本侧已关闭，或传输层失败（对端关闭、对端复位、描述符被关闭、等可写期间被
         *         关闭）：**消息没有发出去，调用方应停止继续发送并收手**；该失败不抛异常，
         *         会话已记下一条中文日志
         * @note text 指向的字节必须活到本次 co_await 结束：协程到首次 resume 才读入参
         */
        Core::Task<bool> sendText(std::string_view text);

        /**
         * @brief 发送一条二进制消息
         * @param payload 负载字节，可含 NUL 与任意二进制
         * @return true 整帧已交给连接
         * @return false 本侧已关闭，或传输层失败（同 sendText()）：**消息没有发出去，调用方应停止
         *         继续发送并收手**；该失败不抛异常
         * @note payload 的存活要求同 sendText()
         */
        Core::Task<bool> sendBinary(std::string_view payload);

        /**
         * @brief 发送一个 Ping 帧
         * @param payload 心跳负载，可为空；不得超过 125 字节（RFC 6455 §5.5）
         * @return true 整帧已交给连接
         * @return false 本侧已关闭，或传输层失败（同 sendText()）：**帧没有发出去，调用方应停止
         *         继续发送并收手**；该失败不抛异常
         * @throws Base::InvalidArgumentException 负载超过控制帧上限：用法错误仍抛异常，编码层当场拒绝
         */
        Core::Task<bool> sendPing(std::string_view payload = {});

        /**
         * @brief 发起关闭握手：发送一个 Close 帧并让本侧不再收发
         *
         * @details 负载为「2 字节大端状态码 + 原因」（RFC 6455 §5.7.1）。控制帧整体不得超过
         *          125 字节，因此原因最长 123 字节：超长时**抛异常而不是截断**——截断会悄悄改掉
         *          业务给出的关闭原因，而这条帧正是对端判断「为什么被关」的唯一依据。
         * @param code 关闭状态码，默认 1000（正常关闭）
         * @param reason 关闭原因文本，可为空；按「指针 + 长度」取
         * @return true Close 帧已交给连接
         * @return false 本侧已发过 Close，或传输层失败（对端已断开或连接不可用）：本侧仍按已关闭
         *         处理，调用方无需重试，也不应再调 send*()；该失败不抛异常
         * @throws Base::InvalidArgumentException 原因超过 123 字节：用法错误仍抛异常
         * @note 调用后 isOpen() 即为 false：本侧已发起关闭，不再发送任何数据帧
         */
        Core::Task<bool> close(std::uint16_t code = kWebSocketNormalClosureCode, std::string_view reason = {});

        /**
         * @brief 本侧是否仍可收发
         * @return true 连接仍打开；false 已发起关闭、已收到对端 Close，或连接不可用
         */
        [[nodiscard]] bool isOpen() const noexcept;

        /**
         * @brief 会话侧：把一段网络字节喂进解码器
         *
         * @details 本段字节一定被全部消费：解码器产出一帧就取走并排队，剩下的字节接着解，
         *          因此调用方不必按 consumedByteCount() 记账，喂完整段即可。文本帧在这一步按整条
         *          消息校验 UTF-8（RFC 6455 §5.6）：消息在交付前已由解码层重组完整，故不需要跨分片增量校验。
         * @param data 数据起始指针，调用方保证可读
         * @param length 数据长度，单位字节
         * @return WebSocketFeedStatus::Accepted 本段已处理完
         * @return WebSocketFeedStatus::DecodeError 对端违反 RFC 6455——帧解码失败，或文本负载不是
         *         合法 UTF-8；调用方应按 decodeErrorCloseCode() 发 Close 并收口
         * @note 已收口的连接不再解码：对端在 Close 之后发来的帧一律丢弃
         */
        WebSocketFeedStatus feedBytes(const char *data, std::size_t length);

        /**
         * @brief 会话侧：标记连接收口，此后不再交付消息
         * @note 有意不唤醒挂起中的 receive()：会话收尾时业务协程的帧会随之销毁，
         *       让它在别人的栈上继续跑没有任何意义
         */
        void markClosed() noexcept;

        /**
         * @brief 会话侧：当前是否有帧正在写
         * @details 会话据此决定收尾时要做什么：业务仍有一帧在写时插一条 Close 会让两条写路径的
         *          字节在连接上互相穿插，此时直接结束会话（由 TCP 收口）。
         * @return true 有一帧的写出尚未完成
         */
        [[nodiscard]] bool isWriteInFlight() const noexcept;

        /**
         * @brief 会话侧：把最近一次 feedBytes() 的 DecodeError 映射成要发的关闭状态码
         * @details 三类失败对端的处置不同，不能合并：文本负载非法回 1007（数据有问题但格式没违规）、
         *          体量越界回 1009（可改用分片重试）、其余协议违规回 1002（RFC 6455 §7.4.1）。
         * @return std::uint16_t 关闭状态码：1007 / 1009 / 1002
         * @note 未发生过 DecodeError 时返回值无意义，调用方应按 WebSocketFeedStatus 判定
         */
        [[nodiscard]] std::uint16_t decodeErrorCloseCode() const noexcept;

        /**
         * @brief 会话侧：最近一次解码失败的中文原因
         * @return 面向使用者的中文描述；无失败时为空串
         */
        [[nodiscard]] std::string decodeErrorText() const;

    private:
        /**
         * @brief receive() 的等待体：等一条排队中的帧，或等连接收口
         */
        class DeliveryAwaiter
        {
        public:
            /**
             * @brief 构造等待体
             * @param peer 所属对端对象，其生命周期必须覆盖本次等待（两者同活在会话帧里）
             */
            explicit DeliveryAwaiter(WebSocketPeer &peer) noexcept;

            /**
             * @brief 队列非空或连接已收口时就地完成，不挂起
             * @return true 可立即取结果
             */
            [[nodiscard]] bool await_ready() const noexcept;

            /**
             * @brief 把本协程登记为接收方，等会话交付或收口时恢复
             * @param waiter 当前协程句柄
             */
            void await_suspend(std::coroutine_handle<> waiter) noexcept;

            /**
             * @brief 取走队首帧
             * @return 帧；队列为空（连接已收口）时为空 optional
             */
            [[nodiscard]] std::optional<WebSocketFrame> await_resume();

        private:
            WebSocketPeer *m_peer; ///< 所属对端对象（非拥有，随会话帧存活）
        };

        /**
         * @brief 编码并写出一帧（send*() 与关闭握手共用）
         * @param opCode 操作码
         * @param payload 负载，按「指针 + 长度」取
         * @return true 整帧已写出
         * @return false 本侧已关闭，或传输层失败（对端关闭、对端复位、描述符被关闭）：后者由发送
         *         回调折成 false 并记日志，不抛异常，本层据此把本侧标记为不可用
         * @throws Base::InvalidArgumentException 用法错误仍抛异常：编码层拒绝（控制帧超长、控制帧要求分片）
         */
        Core::Task<bool> sendFrame(WebSocketOpCode opCode, std::string_view payload);

        /**
         * @brief 把刚解出的帧排队，并在业务正挂起时把控制权交给它
         * @param frame 已解出的帧，负载按移动移交
         */
        void enqueueFrame(WebSocketFrame frame);

        /**
         * @brief 处理一条 Close 帧：回一条同状态码的 Close 并让本侧关闭（RFC 6455 §5.5.1）
         * @param payload 对端 Close 帧的负载，可能为空或只有状态码
         */
        Core::Task<> echoCloseFrame(std::string_view payload);

        FrameSender m_frameSender;                   ///< 发送路径，由会话注入
        HttpMetricsCollector *m_metrics{nullptr};    ///< 统计采集端（非拥有）；空表示不上报 WebSocket 各项计数
        WebSocketFrameDecoder m_decoder;             ///< 帧解码器：掩码校验、分片重组都在它内部完成
        std::deque<WebSocketFrame> m_incomingFrames; ///< 已解出、等待业务取走的帧（FIFO）
        std::coroutine_handle<> m_deliveryWaiter{};  ///< 业务正挂在 receive() 上的句柄，空表示无人等待
        bool m_isOpen{true};                         ///< 本侧是否仍可收发：关闭握手或连接不可用即置 false
        bool m_isWriteInFlight{false};               ///< 是否有帧正在写，供会话收尾判定（见 isWriteInFlight()）
        std::string m_payloadErrorMessage;           ///< 文本负载非法的中文原因（含违规字节位置）；空表示最近一次失败不是负载非法

        /// 本次关闭是对端 Close 的应答：一次对端发起的关闭只记在对端一侧，
        /// 回帧不再重复记成本侧发起。粘性标记——对端关闭后本对象即收口，不存在需要复位的下一轮
        bool m_isEchoingPeerClose{false};
    };
} // namespace AsynGyanis::Net
