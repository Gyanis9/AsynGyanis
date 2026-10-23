/**
 * @file WebSocketPeer.h
 * @brief WebSocket 对端对象：101 之后交给业务处理器的收发句柄（RFC 6455 §5）
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Coroutine/Task.h"
#include "Core/Tls/TlsPeerIdentity.h"
#include "Net/WebSocket/PerMessageDeflate.h"
#include "Net/WebSocket/WebSocketFrame.h"

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Core
{
    class CoroutineResumeTicket;
    class EventLoop;
} // namespace AsynGyanis::Core

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

    /// 关闭类状态码（正常 / 协议错误 / 负载非法）定义在 WebSocketFrame.h：收侧判定两端共用一处；
    /// 消息过大那一条只有本层用，留在这里
    /// 消息过大状态码：单帧或重组后的消息超出解码层上限
    inline constexpr std::uint16_t kWebSocketMessageTooBigCode = 1009;

    /// 策略违规状态码：对端行为超出本端接受范围（如长期不消费导致的收帧积压）
    inline constexpr std::uint16_t kWebSocketPolicyViolationCode = 1008;

    /// 每帧的固定开销估算（帧对象 + 容器节点 + 负载串头）：上界按「负载 + 本开销」记账，
    /// 否则对端连发零负载帧即可绕过（6 字节线上数据换 40~64 字节堆内存，计数却始终为零）
    inline constexpr std::size_t kWebSocketFrameOverheadByteCount = 64;

    /// 待交付帧的积压上界（字节，按负载计）：消费者慢于对端时内存不能被无界吃光
    inline constexpr std::size_t kWebSocketMaximumQueuedPayloadByteCount = 16 * 1024 * 1024;

    /**
     * @brief 一条完整的数据消息
     *
     * @details 分片消息已在解码层重组，因此一条消息恰好对应一次 receive() 返回；
     *          opCode 只可能是 Text 或 Binary，控制帧不会走到业务手里。
     */
    struct ASYN_NET_API WebSocketMessage
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
     *       同一条规则也对着发出的一侧判：`sendText()` 对非法负载当场抛用法错误（见其 @throws），
     *       不会把一帧本端自己会打死的东西交出去。
     *
     * @note 线程约束：全部方法都只在所属事件循环线程上调用，内部状态不加锁。
     * @note 共享所有权：会话用 shared_ptr 持有本对象时经 setLifetimeOwner() 登记一份，集线器订阅后
     *       据此在写协程里续命；栈上对象不登记，寿命仍由调用方按原契约保证。
     * @warning 会话收尾（读到 EOF、解码失败、业务返回）会把本对象标记为关闭：此后 send*() 一律
     *          返回 false（本侧已收口，不再新增日志）、receive() 一律返回空。挂起在 receive() 上的业务协程不会被唤醒，
     *          它的帧随会话一起销毁，因此处理器不能把「收到空结果」当作唯一的退出通知。
     */
    class ASYN_NET_API WebSocketPeer
    {
    public:
        /**
         * @brief 帧发送回调：把一整帧已编码的字节写到这条连接
         *
         * @details 由会话在构造本对象时注入（装配风格与 HttpResponse::ChunkSender 一致）。false = 本帧
         *          未发出、连接不可再用、调用方应停止发送，两种来源：本侧已收口（此前必已记录过收口
         *          原因，本次不再记）与传输失败（对端关闭或复位、描述符被关闭、等可写期间被关闭，本次记
         *          一条中文日志）。失败一律折成 false 不抛异常；回调是协程，写不下时挂起等待可写。
         */
        using FrameSender = std::function<Core::Task<bool>(std::string_view)>;

        /**
         * @brief 会话注入的「重刷这条连接的空闲截止」动作
         *
         * @details 通知里不带时长：会话拿 idleBudgetWithin() 自己按本相位重算，于是「这一刻该不该收掉
         *          这条连接」的算法始终只有一份出处。对端对象在武装心跳、撤掉心跳表、以及写完一帧
         *          （发送回调刚把截止按「等可写」的相位时限刷走）之后各调一次。
         */
        using IdleDeadlineRefresh = std::function<void()>;

        /**
         * @brief 构造对端对象并注入发送路径与统计采集端
         * @param frameSender 发送回调，由会话装配；本对象没有「未装配」状态，必须给出可用的回调
         * @param metrics 统计采集端；传空指针表示本对象不上报 WebSocket 各项计数。它由服务器持有、
         *        按 shared_ptr 共享，本对象只借用，因此比它活得久也不会写到已释放对象上
         */
        explicit WebSocketPeer(FrameSender frameSender, HttpMetricsCollector *metrics = nullptr);

        /**
         * @brief 构造对端对象并同时交入所属事件循环
         * @details 与两参构造的区别只在于 receiveFor() 能不能挂超时定时器：没有循环时只能用
         *          无超时的 receive()，调用 receiveFor() 会在协程内抛出用法错误。
         * @param frameSender 发送回调，由会话装配
         * @param metrics 统计采集端（可空）
         * @param loop 所属事件循环；生命周期必须覆盖本对象
         */
        WebSocketPeer(FrameSender frameSender, HttpMetricsCollector *metrics, Core::EventLoop &loop);

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
         * @brief 会话侧：登记本对象的共享所有权，供集线器在写协程里续命
         *
         * @details 会话用 shared_ptr 持有本对象时把那份所有权登记进来；集线器订阅时经
         *          lifetimeOwner() 取一份，于是写协程挂起期间即使会话先收口并释放它那一份，
         *          本对象也不会先析构。栈上构造的对象不必登记，调用方仍按原契约保证寿命。
         * @param owner 本对象的共享所有权；传空即清除登记
         */
        void setLifetimeOwner(const std::shared_ptr<WebSocketPeer> &owner) noexcept
        {
            m_lifetimeOwner = owner;
        }

        /**
         * @brief 会话侧：取回登记过的共享所有权
         *
         * @details 集线器订阅时调用。栈上对象（未登记或登记已被清除）返回空，调用方仍按原契约
         *          保证对端寿命。
         * @return std::shared_ptr<WebSocketPeer> 已登记且仍存活时的共享所有权；否则为空
         */
        [[nodiscard]] std::shared_ptr<WebSocketPeer> lifetimeOwner() const noexcept
        {
            return m_lifetimeOwner.lock();
        }

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
         * @brief 在时限内收下一条数据消息
         * @details 语义与 receive() 相同，只是等待被 timeout 截断：到点返回空，连接保持可用。
         *          用于「首条消息必须带认证」这类业务——不要为了一个 5 秒窗口单开计时线程。
         *          控制帧（Ping/Pong/Close）照旧在内部处理；反复到达的 Ping 不会续命整个时限，
         *          时限从调用时刻算起，不随中途的控制帧重置。
         * @param timeout 整次接收的时限；非正数表示只做一次非阻塞检查
         * @return 消息；std::nullopt 表示超时、对端关闭或连接不可用。调用方用 isOpen() 区分：
         *         仍为 true 表示这次是超时，false 表示连接已收口
         * @throws Base::LogicException 构造本对象时没有给 EventLoop，或当前不在该循环线程上
         * @note 必须在构造时给的 EventLoop 线程上调用；与 receive() 同一位置，业务是顺序执行的
         */
        Core::Task<std::optional<WebSocketMessage>> receiveFor(std::chrono::milliseconds timeout);

        /**
         * @brief 发送一条文本消息
         * @param text 文本内容，按「指针 + 长度」取，必须是一段合法 UTF-8
         * @return true 整帧已交给连接
         * @return false 本帧未发出、连接不可再用、调用方应停止继续发送并收手；来源有两种：
         *         本侧已收口（此前必已记录过收口原因，本次不新增日志）与传输失败（对端关闭、
         *         对端复位、描述符被关闭、等可写期间被关闭，本次记一条中文日志）。该失败不抛异常；
         *         「false 且日志里没有新记录」通常意味着本侧已收口，排查请回看收口那一刻的日志
         * @throws Base::InvalidArgumentException 用法错误：负载不是合法 UTF-8（RFC 6455 §5.6 要求文本帧
         *         的负载是 UTF-8 文本，报错带违规字节的下标）。本端的接收路径对带非法序列的文本帧是按
         *         1007 打死连接的，自己不收的帧不发；要发任意字节用 `sendBinary()`。这条抛出与
         *         `close()` 的两道用法错误检查同形，不影响上面「false」的两种来源
         * @note text 指向的字节必须活到本次 co_await 结束：协程到首次 resume 才读入参
         * @note 被拒的调用不改变连接状态：本端仍可继续收发
         */
        Core::Task<bool> sendText(std::string_view text);

        /**
         * @brief 发送一条二进制消息
         * @param payload 负载字节，可含 NUL 与任意二进制
         * @return true 整帧已交给连接
         * @return false 本帧未发出、连接不可再用、调用方应停止继续发送并收手（两种来源与日志口径
         *         同 sendText()）；该失败不抛异常
         * @note payload 的存活要求同 sendText()
         */
        Core::Task<bool> sendBinary(std::string_view payload);

        /**
         * @brief 发送一个 Ping 帧
         * @param payload 心跳负载，可为空；不得超过 125 字节（RFC 6455 §5.5）
         * @return true 整帧已交给连接
         * @return false 本帧未发出、连接不可再用、调用方应停止继续发送并收手（两种来源与日志口径
         *         同 sendText()）；该失败不抛异常
         * @throws Base::InvalidArgumentException 负载超过控制帧上限：用法错误仍抛异常，编码层当场拒绝
         */
        Core::Task<bool> sendPing(std::string_view payload = {});

        /**
         * @brief 发起关闭握手：发送一个 Close 帧并让本侧不再收发
         *
         * @details 负载为「2 字节大端状态码 + 原因」（RFC 6455 §5.7.1）。控制帧整体不得超过
         *          125 字节，因此原因最长 123 字节：超长时抛异常而不是截断——截断会悄悄改掉
         *          业务给出的关闭原因，而这条帧正是对端判断「为什么被关」的唯一依据。
         * @param code 关闭状态码，默认 1000（正常关闭）
         * @param reason 关闭原因文本，可为空；按「指针 + 长度」取，必须是一段合法 UTF-8
         * @return true Close 帧已交给连接
         * @return false 本帧未发出、连接不可再用：两种来源与日志口径同 sendText()，而已发过 Close 属
         *         正常收口，未必有单独日志。本侧仍按已关闭处理，调用方无需重试，也不应再调 send*()；
         *         该失败不抛异常
         * @throws Base::InvalidArgumentException 用法错误仍抛异常，三种：原因超过 123 字节；原因不是
         *         合法 UTF-8（RFC 6455 §7.4 要求状态码之后的正文按 UTF-8 编码，而本端的接收路径对带
         *         非法原因的 Close 帧就按 1007 收口——自己不收的帧不发出去）；状态码不允许出现在线上
         *         （1005/1006/1015 是保留哨兵值，1016–2999 段未经注册，见 RFC 6455 §7.4.1/§7.4.2），
         *         可用的取值是 1000–1003、1007–1014 与 3000–4999。本方法是惰性协程，校验与抛出都发生在
         *         返回的任务被驱动时
         * @note reason 指向的字节必须活到本次 co_await 结束：协程到首次 resume 才读入参
         * @note 调用后 isOpen() 即为 false：本侧已发起关闭，不再发送任何数据帧
         */
        Core::Task<bool> close(std::uint16_t code = kWebSocketNormalClosureCode, std::string_view reason = {});

        /**
         * @brief 本侧是否仍可收发
         * @return true 连接仍打开；false 已发起关闭、已收到对端 Close，或连接不可用
         */
        [[nodiscard]] bool isOpen() const noexcept;

        /**
         * @brief 对端 Close 帧交来的状态码原值
         *
         * @details 关闭码的存在意义就是让对端说出「为什么关」：1000 是收工、1001 是离开、
         *          1008 是「按策略拒绝了你」、1011 是对方内部出错。此前这些号码只用来决定本侧回什么，
         *          交回业务的只剩「连接关了」这一件事，业务无从分档收尾（该不该重试、要不要告警、
         *          要不要记审计，全看这个号码）。
         *          交回的是原值，包括本规范禁止上线的取值（1004、1005、1006、1015、5000 及以上）：
         *          那种情况下本侧的回帧按协议错误收口（1002），而「对端到底说了什么」仍然留得住。
         * @return std::optional<std::uint16_t> 有值＝对端交来了状态码；空＝对端没交出号码——
         *         Close 帧只带空负载，或链路被直接切断（那样的帧根本没到过）
         * @note 只在所属事件循环线程上调用；对端的 Close 到达之前恒为空
         */
        [[nodiscard]] std::optional<std::uint16_t> remoteCloseCode() const noexcept;

        /**
         * @brief 对端 Close 帧交来的原因文本
         *
         * @details 只有当它是合法 UTF-8 时才交回：原因不是 UTF-8 时这条关闭按负载非法收口
         *          （RFC 6455 §7.4.1 的 1007），而那串原始字节不交给业务——否则它一旦被写进日志，
         *          控制字节就进了本进程的行（要落日志请先过 Base::escapeForLog）。
         * @return std::string_view 指向本对象持有的文本；对端没给原因时为空串。
         *         有效期到本对象析构为止
         * @note 只在所属事件循环线程上调用
         */
        [[nodiscard]] std::string_view remoteCloseReason() const noexcept;

        /**
         * @brief 设置承载连接的对端证书身份快照
         * @param identity 握手时抓取的快照；明文连接或未启用 mTLS 时为空
         * @note 会话在把本对象交给业务之前调用一次；设置后本对象不再改写
         */
        void setTlsPeerIdentity(Core::TlsPeerIdentityPtr identity) noexcept;

        /**
         * @brief 承载连接的对端证书身份快照
         * @return 对端叶证书身份；明文连接、未启用 mTLS 或尚未设置时为空
         * @note 只在所属事件循环线程上调用；身份对象只读
         */
        [[nodiscard]] const Core::TlsPeerIdentityPtr &peerIdentity() const noexcept;

        /**
         * @brief 会话侧：交出 permessage-deflate 的协商结论（RFC 7692）
         *
         * @details 有值即启用：sendText()/sendBinary() 自动压缩负载并置 RSV1，收到的压缩消息自动解压；
         *          控制帧从不参与压缩（§6.1）。空表示本端没接受这个扩展，此时 RSV1 是保留位、
         *          出现即按协议错误收口。必须在喂入任何字节之前调用：解码器已开始工作时
         *          改这个开关，会让同一条消息的前后判断不一致。
         *          窗口位数随结论一起交回而不是让本端自己挑一个：那两位是对端声明的能力，
         *          压缩位数超出它会让对端解不开（协商那侧已经钳好了，这里只负责把它带到收发点上）。
         * @param window 协商出来的两侧窗口位数；nullopt 表示未就该扩展与对端达成一致
         */
        void setPerMessageDeflate(std::optional<PerMessageDeflateWindow> window) noexcept;

        /**
         * @brief 会话侧：设定本端愿意收的一条入站消息字节上限
         *
         * @details 会话按 `HttpParserLimits::maximumWebsocketMessageSize` 在建对端对象时交下来，
         *          一个数管三处：单帧声明的长度、分片重组后的总长、以及 permessage-deflate 的解压输出上限
         *          ——按不同尺子判这三处，等于给对端留了一条「换个写法就绕过闸门」的缝。
         *          0 表示不设上限（与 HttpParserLimits 各项的 0 语义一致）。
         *          必须在喂入任何字节之前调用：与压缩开关同一条理由，同一条消息的前后两段不能按两把尺子判。
         * @param maximumInboundMessageBytes 一条消息的字节上限；0 表示不设上限
         */
        void setMaximumInboundMessageBytes(std::size_t maximumInboundMessageBytes) noexcept;

        /**
         * @brief 会话侧：装配「本端发出 Ping 后多久没收到 Pong 就判这条连接已死」
         *
         * @details 计时起点是本端把这一帧 Ping 写出去之后，与「收到过字节」无关：一直发数据却不回 Pong
         *          的对端同样判死——RFC 6455 §5.5.3 要求收到 Ping 后尽快回 Pong，那是唯一的应用层存活证据。
         *          本对象只记下截止时刻并按时机请会话重刷那条截止（见 IdleDeadlineRefresh），不自持定时器也不
         *          新起线程：到点由各通道既有的那一拍落实（h1/h2 的连接空闲清扫、h3 的会话到期节拍）。
         * @param timeout 时限，单位毫秒；0 表示不设这项保护
         * @param idleDeadlineRefresh 会话注入的重刷动作，见 IdleDeadlineRefresh 的说明；h1/h2 用它把连接
         *        的空闲截止折短，h3 逐条查 isPongDeadlinePassedAt()，不给这个动作
         * @note 同一时刻只记最早那条未被应答的 Ping（后发的心跳不把截止往后推），收到任何一帧 Pong 即撤表
         */
        void setPongTimeout(std::chrono::milliseconds timeout, IdleDeadlineRefresh idleDeadlineRefresh = {});

        /**
         * @brief 把相位时限与在途心跳的剩余预算合成一个要刷给连接空闲截止的时长
         * @details 没有在途心跳时原样交回 phaseTimeout，因此这条通道上没发过心跳的连接与本时限无关。
         * @param phaseTimeout 本相位的常规时限；0 表示那一侧不设时限
         * @param now 比较时刻（由调用方给出，使这条换算能被确定性地测出来）
         * @return std::chrono::milliseconds 两者里更早的那一个；已过期时给 1 毫秒（0 是「清除截止时间」，
         *         会被清扫协程当成不设时限放行）
         */
        [[nodiscard]] std::chrono::milliseconds idleBudgetWithin(std::chrono::milliseconds phaseTimeout, std::chrono::steady_clock::time_point now) const noexcept;

        /**
         * @brief 在途心跳是否已经过了截止时刻
         * @details 过点之后由调用方收口：h1/h2 的会话在读循环退出时据此把死因写成「心跳没回」而不是
         *          「空闲超时」，h3 由会话的到期节拍逐条查它并收掉那一条隧道。
         * @param now 比较时刻（由调用方给出，使这条判定能被确定性地测出来）
         * @return true 有在途 Ping 且已过截止
         * @return false 没有在途 Ping、或未启用这项保护、或还没到截止
         */
        [[nodiscard]] bool isPongDeadlinePassedAt(std::chrono::steady_clock::time_point now) const noexcept;

        /**
         * @brief 会话侧：把一段网络字节喂进解码器
         *
         * @details 本段字节一定被全部消费：解码器产出一帧就取走并排队，剩下的字节接着解，
         *          因此调用方不必按 consumedByteCount() 记账，喂完整段即可。协商过 permessage-deflate 时，
         *          置了 RSV1 的消息先解压再交付（RFC 7692 §7.2.2）。文本帧在这一步按整条消息校验 UTF-8
         *          （RFC 6455 §5.6）：消息在交付前已由解码层重组完整，故不需要跨分片增量校验。
         * @param data 数据起始指针，调用方保证可读
         * @param length 数据长度，单位字节
         * @return WebSocketFeedStatus::Accepted 本段已处理完
         * @return WebSocketFeedStatus::DecodeError 对端违反 RFC 6455——帧解码失败、文本负载不是
         *         合法 UTF-8，或压缩消息解不开；调用方应按 decodeErrorCloseCode() 发 Close 并收口
         * @note 已收口的连接不再解码：对端在 Close 之后发来的帧一律丢弃
         */
        WebSocketFeedStatus feedBytes(const char *data, std::size_t length);

        /**
         * @brief 会话侧：标记连接收口，此后不再交付消息
         * @note 有意不唤醒挂起中的 receive()：会话收尾时业务协程的帧会随之销毁，
         *       让它在别人的栈上继续跑没有任何意义。需要「先唤醒再收尾」的路径调 wakeDeliveryWaiter()
         */
        void markClosed() noexcept;

        /**
         * @brief 会话侧：唤醒挂在 receive() 上的业务，让它以「连接已收口」收尾
         * @note 只在业务协程帧会活到它自己跑完的路径上调用（隧道路径由 reapFinishedTunnels()
         *       兜底）。会话一边收尾一边直接销毁帧的路径不能调它——那是「先销毁后唤醒」
         */
        void wakeDeliveryWaiter() noexcept;

        /**
         * @brief 会话侧：当前是否有帧正在写
         * @details 会话据此决定收尾时要做什么：业务仍有一帧在写时插一条 Close 会让两条写路径的
         *          字节在连接上互相穿插，此时直接结束会话（由 TCP 收口）。
         * @return true 有一帧的写出尚未完成
         */
        [[nodiscard]] bool isWriteInFlight() const noexcept;

        /**
         * @brief 会话侧：把最近一次 feedBytes() 的 DecodeError 映射成要发的关闭状态码
         * @details 各失败对端的处置不同，不能合并：文本负载非法回 1007（数据有问题但格式没违规）、
         *          体量越界回 1009（可改用分片重试）、压缩消息解不开回 1002（RFC 7692 §7.2.2 要求
         *          直接失败连接）、其余协议违规回 1002（RFC 6455 §7.4.1）。
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
         * @brief receiveFor() 的等待体：在「有帧交付」与「定时器到期」之间只恢复一次
         * @details 两条唤醒来源共享一张 CoroutineResumeTicket：投递方取走句柄再恢复，因此不会
         *          两次 resume 同一条协程。定时任务归本等待体所有——帧先到时析构会摘掉队列登记，
         *          定时先到时任务已在 final suspend，析构是空操作。
         */
        class TimedDeliveryAwaiter
        {
        public:
            /**
             * @brief 构造等待体
             * @param peer 所属对端对象
             * @param timeout 整次等待的剩余时限；非正数表示只做一次非阻塞检查
             */
            TimedDeliveryAwaiter(WebSocketPeer &peer, std::chrono::milliseconds timeout);

            /**
             * @brief 析构：撤销票据并销毁定时任务（定时器在挂起中时随之摘除）
             */
            ~TimedDeliveryAwaiter();

            /**
             * @brief 队列非空、连接已收口或时限已到时就地完成
             */
            [[nodiscard]] bool await_ready() noexcept;

            /**
             * @brief 登记票据与定时任务，挂起当前协程
             * @param waiter 当前协程句柄
             * @throws Base::LogicException 构造本对端时没有给 EventLoop
             */
            void await_suspend(std::coroutine_handle<> waiter);

            /**
             * @brief 取走队首帧；超时/收口时为空
             */
            [[nodiscard]] std::optional<WebSocketFrame> await_resume();

        private:
            /**
             * @brief 等 timeout 后把票据投回循环；不在计时任务栈上直接 resume
             * @details 定时任务归等待体所有：直接 resume 会让接收方在恢复后析构本任务帧，
             *          等于在正在执行的协程里 destroy 自己。投回循环让本任务先完成到 final suspend。
             */
            Core::Task<> watchTimeout(std::chrono::milliseconds timeout);

            WebSocketPeer                               *m_peer;
            std::chrono::milliseconds                    m_timeout;
            std::shared_ptr<Core::CoroutineResumeTicket> m_ticket;
            std::optional<Core::Task<>>                  m_timeoutTask;
        };

        /// consumeFrame() 的处置：继续等下一条、连接已收口、或交出一条数据消息
        enum class FrameDisposition
        {
            Continue,
            Closed,
            Message
        };

        /**
         * @brief 处理一条已出队的帧：心跳就地应答/撤钟，Close 回帧并收口，数据帧交回业务
         * @param frame 已从队列取走的帧
         * @return 处置与可选的数据消息；Continue 时消息为空，Closed 时消息为空
         */
        Core::Task<std::pair<FrameDisposition, std::optional<WebSocketMessage>>> consumeFrame(WebSocketFrame frame);

        /**
         * @brief 编码并写出一帧（send*() 与关闭握手共用）
         * @param opCode 操作码
         * @param payload 负载，按「指针 + 长度」取
         * @return true 整帧已写出
         * @return false 本帧未发出、连接不可再用（两种来源与日志口径见 sendText()）：本层据此把本侧
         *         标记为不可用，此后 send*()/close() 一律短路返回 false 且不再记日志
         * @throws Base::InvalidArgumentException 用法错误仍抛异常：编码层拒绝（控制帧超长、控制帧要求分片）
         */
        Core::Task<bool> sendFrame(WebSocketOpCode opCode, std::string_view payload);

        /**
         * @brief 把刚解出的帧排队，并在业务正挂起时把控制权交给它
         * @param frame 已解出的帧，负载按移动移交
         */
        void enqueueFrame(WebSocketFrame frame);

        /**
         * @brief 处理一条 Close 帧：校验负载后回 Close 并让本侧关闭（RFC 6455 §5.5.1、§7.4.1）
         * @param payload 对端 Close 帧的负载，可能为空、只有状态码或带原因文本
         * @details 负载非法（1 字节、禁止上线的状态码、原因不是 UTF-8）时不回送对端的码，
         *          改按 1002/1007 收口——原样回送会把一个非法关闭当成正常关闭放过去。
         *          同时把对端交来的状态码原值与合法 UTF-8 的原因记下，交给 remoteCloseCode() /
         *          remoteCloseReason() 读回业务（回帧用的是另一个码，两件事各自留档）
         */
        Core::Task<> echoCloseFrame(std::string_view payload);

        /**
         * @brief 武装在途心跳：记下截止时刻并请会话重刷连接的空闲截止
         * @details 未启用（时限为 0）或有在途 Ping 时什么都不做：后者不把最早那条的截止往后推，
         *          否则连发心跳的处理器会把一把判死的钟一圈圈续走。
         */
        void armPongDeadline();

        /**
         * @brief 撤掉心跳看门狗并通知会话
         * @details 撤表也要通知：此前被刷短的空闲截止得按相位时限还回去，否则一条健康而安静的
         *          连接会被那把心跳钟提前收掉。
         */
        void clearPongDeadline();

        /// 请会话按本相位重算并刷回连接的空闲截止；未注入该动作时是空操作
        void refreshSessionIdleDeadline() const;

        /// 会话登记的共享所有权：集线器在写协程挂起期间靠它续住本对象；空表示栈上对象或未登记
        std::weak_ptr<WebSocketPeer> m_lifetimeOwner{};

        FrameSender                m_frameSender;       ///< 发送路径，由会话注入
        HttpMetricsCollector      *m_metrics{nullptr};  ///< 统计采集端（非拥有）；空表示不上报 WebSocket 各项计数
        Core::TlsPeerIdentityPtr   m_tlsPeerIdentity{}; ///< 承载连接的对端证书身份；明文或未设置时为空
        Core::EventLoop           *m_loop{nullptr};     ///< 所属事件循环（非拥有）；receiveFor() 的定时器挂在它上面，空表示未注入
        WebSocketFrameDecoder      m_decoder;           ///< 帧解码器：掩码校验、分片重组都在它内部完成
        std::deque<WebSocketFrame> m_incomingFrames;    ///< 已解出、等待业务取走的帧（FIFO）
        /// 待交付帧的积压计数：enqueueFrame() 累加、出队处扣减，两处必须配对
        std::size_t             m_queuedPayloadByteCount{0}; ///< 队列里待交付帧的负载总字节数
        std::coroutine_handle<> m_deliveryWaiter{};          ///< 业务正挂在 receive() 上的句柄，空表示无人等待
        /// receiveFor() 的恢复票据：定时器与帧交付只允许一方 resume。与 m_deliveryWaiter 互斥使用
        std::shared_ptr<Core::CoroutineResumeTicket> m_deliveryTicket{};
        bool                                         m_isOpen{true};           ///< 本侧是否仍可收发：关闭握手或连接不可用即置 false
        bool                                         m_isWriteInFlight{false}; ///< 是否有帧正在写，供会话收尾判定（见 isWriteInFlight()）
        std::string                                  m_payloadErrorMessage;    ///< 负载层失败的中文原因（文本非法含违规字节位置）；空表示最近一次失败不在负载层
        std::uint16_t                                m_payloadErrorCloseCode{kWebSocketInvalidPayloadDataCode}; ///< 负载层失败对应的关闭状态码
        std::optional<PerMessageDeflateWindow>       m_deflateWindow{}; ///< 协商出的两侧窗口位数；空表示未协商 permessage-deflate，决定收发两侧是否压缩

        std::chrono::milliseconds m_pongTimeout{};         ///< 「发出 Ping 后多久没收到 Pong」的时限；0 表示不设这项保护
        IdleDeadlineRefresh       m_idleDeadlineRefresh{}; ///< 会话注入的「重刷本连接空闲截止」动作；空表示这条通道不靠它落实时限
        /// 最早那条未被应答 Ping 的截止时刻；空表示没有在途心跳（没发过 Ping、Pong 已回、或本通道未装配时限）
        std::optional<std::chrono::steady_clock::time_point> m_outstandingPongDeadline{};

        /// 本次关闭是对端 Close 的应答：一次对端发起的关闭只记在对端一侧，
        /// 回帧不再重复记成本侧发起。粘性标记——对端关闭后本对象即收口，不存在需要复位的下一轮
        bool m_isEchoingPeerClose{false};

        /// 对端 Close 帧里交来的状态码原值（见 remoteCloseCode()）：对端没交码、或链路被直接切断时保持空
        std::optional<std::uint16_t> m_remoteCloseCode{};
        /// 对端 Close 帧里交来的原因文本，只在它是合法 UTF-8 时留下（见 remoteCloseReason()）
        std::string m_remoteCloseReason{};
    };
} // namespace AsynGyanis::Net
