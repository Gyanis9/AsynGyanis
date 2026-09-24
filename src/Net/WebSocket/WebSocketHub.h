/**
 * @file WebSocketHub.h
 * @brief WebSocket 扇出集线器：按主题登记成员，一条消息发给全员，慢成员有界而不打爆内存
 * @author Gyanis
 * @date 2026-09-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 会话层只提供「一条连接自己收发」，广播要额外解决三件事，本类把这三件都管住：
 *          1. **成员生存期**：集线器不能持有比处理器更久的对端指针。订阅是 RAII 句柄，
 *             句柄析构（处理器 return、异常展开、或显式 reset）即除名，不留悬垂指针这一类脚枪。
 *          2. **一条连接一个写者**：两条不同的连接同时向同一个成员发，会让两帧字节在线上互相穿插。
 *             成员自带「谁在替它写」的闩：已有写者在跑时，后来者只入队，不另起一条写路径。
 *          3. **慢读者的内存**：队列有字节上界，越界丢**最新**的一条并计数——保住已入队的顺序，
 *             也不让一条不消费的连接把整机内存吃光。丢了多少条从 droppedMessageCount() 读得到。
 *
 *          跨循环的部署要按「一条事件循环一个集线器」来装：本类的所有方法都只在所属循环上调用，
 *          内部不加锁（与 WebSocketPeer 同一口径）。要把消息送到别的循环上的成员，
 *          由业务侧 `loop.scheduler().postRemote(...)` 投到那条循环再在那边 publish。
 * @see WebSocketPeer
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Net/WebSocket/WebSocketPeer.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief 一次订阅的标识：0 保留给「空句柄」，其余由集线器从 1 起递增发号
     * @details 用标识而不是迭代器定位成员：扇出过程中表会被并发改写（别的连接订阅/除名），
     *          迭代器会失效，而标识只是把「找不到」当成「已被别人先摘掉」。
     */
    using WebSocketSubscriptionId = std::uint64_t;

    namespace detail
    {
        /**
         * @brief 集线器的一个成员：对端指针、待发队列、与「谁在替它写」的闩
         * @details 生存期由集线器与订阅句柄共持 shared_ptr：这样即使成员在某个 publish 协程
         *          挂起期间被除名，那份队列与闩也还活着， drain 能干净收尾而不是踩在被删对象上。
         *          对端指针则由除名时清空——它才是那个会悬垂的东西。
         */
        struct WebSocketHubMember
        {
            WebSocketPeer *peer{nullptr};   ///< 空表示已除名或已收口：此后不再碰这条连接
            std::deque<std::string> pendingTexts; ///< 已入队、尚未写出的消息，按到达顺序
            std::size_t pendingByteCount{0};      ///< pendingTexts 的负载总字节数（入队上界据此判定）
            bool isDraining{false};               ///< 是否已有一个发布协程正在替它写（一条连接一个写者）
        };
    } // namespace detail

    class WebSocketHub;

    /**
     * @brief 一次主题订阅的 RAII 句柄：析构即除名
     *
     * @details 只能移动、不能拷贝：一份订阅对应集线器里的一个成员，复制出两份就会有两处除名点。
     * @warning 句柄必须比它的对端连接活得短。会话侧的构造顺序本就满足这件事（对端对象在业务
     *          协程帧之外构造、之内销毁），把句柄放在处理器自己的栈上即可，不要把它存到别处。
     */
    class WebSocketSubscription
    {
    public:
        /**
         * @brief 造一个空句柄（未订阅）：reset() 与析构对它是无操作
         */
        WebSocketSubscription() noexcept = default;

        /**
         * @brief 析构：仍持有时向集线器除名
         */
        ~WebSocketSubscription();

        WebSocketSubscription(const WebSocketSubscription &) = delete;

        WebSocketSubscription &operator=(const WebSocketSubscription &) = delete;

        WebSocketSubscription(WebSocketSubscription &&other) noexcept;

        WebSocketSubscription &operator=(WebSocketSubscription &&other) noexcept;

        /**
         * @brief 主动除名（等价于提前析构）：连接要收口、或业务要离开主题时调用
         * @note 幂等：空句柄上再调一次什么都不做
         */
        void reset() noexcept;

        /// @brief 本次订阅的标识；空句柄为 0
        [[nodiscard]] WebSocketSubscriptionId id() const noexcept;

        /// @brief 是否仍在集线器里（除名后为 false）
        [[nodiscard]] bool isActive() const noexcept;

    private:
        friend class WebSocketHub;

        /**
         * @brief 由集线器在订阅成功时装配
         * @param hub 所属集线器（不拥有，但句柄析构时一定还活着：见类说明的生存期约束）
         * @param member 集线器里的成员
         * @param identifier 本次订阅的标识
         */
        WebSocketSubscription(WebSocketHub &hub, std::shared_ptr<detail::WebSocketHubMember> member, WebSocketSubscriptionId identifier);

        WebSocketHub *m_hub{nullptr};                            ///< 所属集线器；空句柄为 nullptr
        std::shared_ptr<detail::WebSocketHubMember> m_member{};  ///< 成员（与集线器共持）
        WebSocketSubscriptionId m_id{0};                         ///< 订阅标识，除名按它定位
    };

    /**
     * @brief 按主题扇出的集线器（单条事件循环内使用，见文件说明的线程口径）
     */
    class WebSocketHub
    {
    public:
        /// 单个成员待发队列的默认字节上界：1 MiB 足够盖住一次突发，又远小于可疑慢连接的无界增长
        inline static constexpr std::size_t kDefaultMaximumPendingByteCount = 1U << 20U;

        /**
         * @brief 构造集线器
         * @param maximumPendingByteCount 单成员待发队列的字节上界；必须为正数
         * @throws Base::InvalidArgumentException 用法错误：上界为 0，那样一条消息也进不去
         */
        explicit WebSocketHub(std::size_t maximumPendingByteCount = kDefaultMaximumPendingByteCount);

        // 禁拷贝与移动：订阅句柄按地址记住本对象，复制一份会让除名打到另一份表上
        WebSocketHub(const WebSocketHub &) = delete;

        WebSocketHub &operator=(const WebSocketHub &) = delete;

        WebSocketHub(WebSocketHub &&) = delete;

        WebSocketHub &operator=(WebSocketHub &&) = delete;

        /**
         * @brief 让一条已升级的连接加入主题
         * @details 同一个对端可以多次订阅不同主题（一次一份句柄），加入自己已加入的主题则视为
         *          两个独立成员——本类不做「按对端去重」，那是业务的语义而不是扇出的语义。
         * @param topic 主题名，原样保存（区分大小写，不做归一化）
         * @param peer 本条循环上已升级的对端；其生存期必须覆盖返回句柄的析构
         * @return WebSocketSubscription 订阅句柄；析构即除名
         */
        [[nodiscard]] WebSocketSubscription subscribe(std::string_view topic, WebSocketPeer &peer);

        /**
         * @brief 把一条消息发给该主题的全体成员
         *
         * @details 语义是「尽力达」而不是「已达」：每个成员各自排进自己的队列，队列空闲时本次调用
         *          顺带替它写出去（会挂起）；队列已被别的发布协程占着，就只入队立刻返回。
         *          因此 publish 返回时不保证字节已上线——那条连接随后被关掉，队列里的东西就随它去。
         *          需要逐成员送达确认的场合，请业务自己点对点 sendText，不要用扇出。
         * @param topic 主题名
         * @param text 消息文本，按文本帧发出（UTF-8 校验由对端发送路径负责）
         * @return Core::Task<void> 排完本次扇出即返回
         */
        Core::Task<void> publish(std::string_view topic, std::string_view text);

        /**
         * @brief 该主题当前的成员数
         * @param topic 主题名
         * @return std::size_t 成员条数
         */
        [[nodiscard]] std::size_t memberCount(std::string_view topic) const;

        /// @brief 全部主题的成员总数
        [[nodiscard]] std::size_t subscriptionCount() const noexcept;

        /// @brief 因队列越界而被丢掉的**新**消息累计条数（慢读者的可见证据，不做静默丢弃）
        [[nodiscard]] std::size_t droppedMessageCount() const noexcept;

        /// @brief 单成员待发队列的字节上界
        [[nodiscard]] std::size_t maximumPendingByteCount() const noexcept;

    private:
        friend class WebSocketSubscription;

        /// 一条订阅登记：成员按主题归属，除名按标识定位
        struct Registration
        {
            std::string topic;                                          ///< 主题名
            std::shared_ptr<detail::WebSocketHubMember> member{};       ///< 成员
            WebSocketSubscriptionId identifier{0};                      ///< 订阅标识
        };

        /**
         * @brief 除名：清掉对端指针并把这条登记从表里摘掉
         * @param identifier 订阅标识
         */
        void unsubscribe(WebSocketSubscriptionId identifier);

        /**
         * @brief 替一个成员把队列里的消息写完：一条连接同一时刻只有这一个写者
         * @param member 目标成员（按值持 shared_ptr：挂起期间表可能已经把它摘掉）
         * @return Core::Task<void> 队列空、或对端不可再用时返回
         */
        Core::Task<void> drainMember(std::shared_ptr<detail::WebSocketHubMember> member);

        std::vector<Registration> m_registrations; ///< 全部成员，按订阅顺序
        std::size_t m_maximumPendingByteCount;     ///< 单成员待发队列字节上界
        std::size_t m_droppedMessageCount{0};      ///< 队满丢弃的累计条数
        WebSocketSubscriptionId m_nextIdentifier{1U}; ///< 下一个订阅标识：从 1 起，0 留给「空句柄」
    };
} // namespace AsynGyanis::Net
