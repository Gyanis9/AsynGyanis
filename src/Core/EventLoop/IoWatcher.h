/**
 * @file IoWatcher.h
 * @brief 常驻 epoll 注册：注册一次即可反复等待，关注位只在有等待者时武装
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <coroutine>
#include <cstdint>

namespace AsynGyanis::Core
{
    class EventLoop;

    /**
     * @brief 一个文件描述符的常驻 epoll 注册与等待入口
     *
     * @details 本类把「描述符属于本事件循环」与「现在关注哪些事件」分成两件事：
     *          构造时注册一次（建立归属关系，并借此校验该描述符没有被另一个注册对象占用），
     *          析构时反注册一次；**事件关注位则只在有协程等待期间武装**，并借 EPOLLONESHOT
     *          让内核在一次上报后自动解除。连接存活期间因此可以无限次 `co_await` 其等待器，
     *          而不需要「等一次注册一次」的 ADD/DEL 往返。
     *
     * ## 为什么关注位必须按需武装
     * Windows 侧用的是 wepoll，它没有实现边沿触发（`ASYN_PLATFORM_WIN32` 下 EPOLLET 被定义为 0），
     * 所有注册都是水平触发。水平触发下，凡是「长期为真」的关注位——可写的套接字、已到达却还没被
     * 读走的数据、定时器里没被清掉的到期标记——都会让每一次 epoll_wait 立刻返回该事件。而持有该
     * 关注位的注册对象当时可能根本没有等待者，于是事件循环空转：实测一条空闲连接就能在 2 秒内
     * 产生 10.1 万次可写上报，把一个核吃满。按需武装让「内核在盯着什么」与「现在谁在等」始终一致，
     * 没人等就不盯，空转因此不可能发生。
     *
     * ## 代价
     * 每次真正发生的等待付一次 `epoll_ctl(MOD)`（武装新方向，或等待方向发生变化时改写关注位）；
     * 若该方向已按当前需要武装着，则一次系统调用都不付。等待期间不再有别的 epoll 控制调用。
     *
     * ## 就绪缓存
     * 「已武装后被上报、但此刻没有等待者」这一种情形（等待者恰在事件到达前被销毁）会把就绪
     * **记下来**，留给下一次等待立即完成。除此之外就绪都由内核在每次武装时重新判定——
     * 按需武装让「错过就绪」从一个会丢的边沿变成一次可重新查询的状态。
     *
     * ## 关闭会唤醒等待者
     * 析构时若仍有协程挂在等待器上，它会被投递到事件循环的调度队列（不是就地恢复——本对象
     * 正在析构，就地恢复会让协程在析构未完成时回来访问成员），并以「未就绪」结束等待，
     * 于是等待方可以立刻收尾而不是永久挂起。
     *
     * @note 线程约束：本类与其等待器都只在**所属事件循环线程**上使用。
     *       等待/恢复两端都在该线程，因此内部状态不需要任何原子或锁。
     * @note 一个方向同时只允许一个等待者：同一文件描述符的同一方向上并发等待会抛
     *       `Base::LogicException`（这几乎总是「读协程起了两个」这类用法错误，
     *       静默让某一个永远等不到比当场报错危险得多）。
     */
    class IoWatcher
    {
    public:
        /**
         * @brief 等待器：与其它 awaitable 一样直接 co_await
         */
        class Awaiter
        {
        public:
            /**
             * @brief 构造等待器
             * @param watcher 目标注册对象
             * @param event 关注的事件位（EPOLLIN 或 EPOLLOUT）
             */
            Awaiter(IoWatcher &watcher, std::uint32_t event) noexcept;

            Awaiter(const Awaiter &) = delete;

            Awaiter &operator=(const Awaiter &) = delete;

            /**
             * @brief 析构时把自己从注册对象上摘除
             * @details 协程帧可能在等待期间被销毁（取消、异常展开）。此时等待器对象随帧一起
             *          析构，必须顺手摘除注册，否则注册对象里会留下一个指向已释放等待器的悬空指针。
             */
            ~Awaiter();

            /**
             * @brief 就绪则立即完成，不挂起
             * @return true 该方向的就绪标记已在（见就绪缓存），等待立即完成
             */
            [[nodiscard]] bool await_ready() noexcept;

            /**
             * @brief 登记为本方向的等待者、武装关注位并挂起协程
             * @param handle 当前协程句柄，事件到达时由注册对象恢复
             * @return true 已登记，可以挂起
             * @return false 注册已失效（描述符已关闭或武装失败），不挂起、立即以「未就绪」结束等待
             * @throws Base::LogicException 该方向已有另一个等待者
             */
            [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle);

            /**
             * @brief 取回等待结果
             * @return true 事件已就绪（调用方应立刻重试系统调用）
             * @return false 注册已失效（描述符已关闭），调用方应停止重试并收尾
             */
            [[nodiscard]] bool await_resume() const noexcept;

        private:
            friend class IoWatcher;

            /**
             * @brief 由注册对象标记本次等待已就绪
             */
            void markReady() noexcept;

            /**
             * @brief 由注册对象告知「你已不在我的登记槽里」
             * @details 注册对象一旦把等待者从槽里取走（无论是为了恢复它，还是它自己在析构），
             *          都必须调用本方法。否则等待器析构时会以为自己还挂在注册对象上，
             *          去调用可能是**已被销毁**的注册对象——即释放后使用。
             */
            void markDetached() noexcept;

            IoWatcher   *m_watcher;        ///< 目标注册对象（非拥有）
            std::uint32_t m_event;         ///< 本次关注的事件位
            bool         m_isAttached{false}; ///< 是否已登记到注册对象上
            bool         m_isReady{false};    ///< 结果：事件是否就绪
        };

        /**
         * @brief 构造并常驻注册一个文件描述符
         * @param loop 所属事件循环（提供 epoll 与调度队列）
         * @param fileDescriptor 目标文件描述符；为负数时得到一个「无效」的注册对象
         *        （等待一律以「未就绪」结束），便于持有空描述符的对象统一处理
         * @throws Base::SystemException 描述符有效但注册失败（通常意味着同一描述符已被
         *         另一个注册对象占用——这是用法错误，当场失败好过等到第一次等待时才暴露）
         * @note 构造时只把「可读」作为一次性探测交给内核，写入方向一律等有人等时才武装：
         *       可读要等真有数据才可能就绪，最多产生一次无害的探测事件；而可写几乎长期为真，
         *       提前武装它只会在没人等待时交付一份陈旧的可写就绪
         */
        IoWatcher(EventLoop &loop, int fileDescriptor);

        /**
         * @brief 析构：反注册，并把仍在等待的协程以「未就绪」唤醒
         */
        ~IoWatcher();

        // 禁止拷贝与移动：注册时写进 epoll 的是本对象的地址，移动会让它失效
        IoWatcher(const IoWatcher &) = delete;

        IoWatcher &operator=(const IoWatcher &) = delete;

        IoWatcher(IoWatcher &&) = delete;

        IoWatcher &operator=(IoWatcher &&) = delete;

        /**
         * @brief 是否已成功注册
         * @return true 已注册到 epoll，等待器可用
         */
        [[nodiscard]] bool isValid() const noexcept;

        /**
         * @brief 获取被注册的文件描述符
         * @return int 文件描述符，无效时为负数
         */
        [[nodiscard]] int fileDescriptor() const noexcept;

        /**
         * @brief 等待该描述符可读（EPOLLIN）
         * @return Awaiter 等待器；已就绪时 co_await 立即返回
         */
        [[nodiscard]] Awaiter waitReadable() noexcept;

        /**
         * @brief 等待该描述符可写（EPOLLOUT）
         * @return Awaiter 等待器；已就绪时 co_await 立即返回
         */
        [[nodiscard]] Awaiter waitWritable() noexcept;

        /**
         * @brief 处理来自事件循环的事件分发
         * @details 仅由 EventLoop 在分发 epoll 事件时调用。若该方向有等待者，就把就绪结果
         *          交给它并恢复它；没有等待者则把就绪记下来留给下一次等待。仍有人在等的
         *          方向会在这里补一次武装（本次上报已消耗掉上一次的一次性关注）。
         * @param events epoll 报告的事件位（含错误与挂断位）
         * @note 恢复协程之后**不再访问任何成员**：被恢复的代码可能立刻销毁本对象
         *       （例如读到对端关闭后关闭连接），那之后访问成员就是释放后使用。
         */
        void handleEvents(std::uint32_t events) noexcept;

    private:
        /**
         * @brief 一个方向上的等待者登记
         */
        struct WaiterSlot
        {
            std::coroutine_handle<> handle{};      ///< 等待中的协程，空表示当前无人等待
            Awaiter                *awaiter{nullptr}; ///< 对应的等待器，用于把结果写回它
        };

        /**
         * @brief 登记一个等待者，并把它的关注位武装到位
         * @param event 事件位（EPOLLIN 或 EPOLLOUT）
         * @param handle 等待中的协程句柄
         * @param awaiter 对应等待器
         * @return true 登记成功，调用方应挂起
         * @return false 注册已失效或武装失败，调用方不应挂起（立即以「未就绪」结束等待）
         * @throws Base::LogicException 该方向已有等待者
         */
        [[nodiscard]] bool attachWaiter(std::uint32_t event, std::coroutine_handle<> handle, Awaiter &awaiter);

        /**
         * @brief 把某个方向的等待者从登记槽摘下，并按结果通知它
         * @param event 事件位（EPOLLIN 或 EPOLLOUT）
         * @param isReady 是否以「就绪」通知（false 表示按未就绪收尾）
         * @return 需要恢复的协程句柄，无人等待时为空
         */
        [[nodiscard]] std::coroutine_handle<> takeWaiter(std::uint32_t event, bool isReady) noexcept;

        /**
         * @brief 摘除某个方向上的等待者登记（等待器析构、等待登记作废时调用）
         * @param event 事件位（EPOLLIN 或 EPOLLOUT）
         */
        void detachWaiter(std::uint32_t event) noexcept;

        /**
         * @brief 把关注位武装到内核（按需）
         * @param events 本次需要关注的事件位；为 0 时不改动内核状态
         * @return true 已按需武装（含「本来就已经武装着」）
         * @return false 武装失败（描述符已失效），调用方应按注册失效处理
         */
        [[nodiscard]] bool armEvents(std::uint32_t events) noexcept;

        /**
         * @brief 取走某个方向的就绪标记
         * @param event 事件位（EPOLLIN 或 EPOLLOUT）
         * @return true 该方向此前已就绪且无人取走
         */
        [[nodiscard]] bool consumeReady(std::uint32_t event) noexcept;

        /**
         * @brief 取某个方向对应的等待者登记槽
         * @param event 事件位（EPOLLIN 或 EPOLLOUT）
         * @return WaiterSlot& 该方向的登记槽
         */
        [[nodiscard]] WaiterSlot &slotFor(std::uint32_t event) noexcept;

        EventLoop    *m_loop;            ///< 所属事件循环（非拥有）
        int           m_fileDescriptor;  ///< 被注册的文件描述符
        bool          m_isRegistered{false}; ///< 是否已成功注册到 epoll
        std::uint32_t m_armedEvents{0};  ///< 当前在核心里武装着的事件位（EPOLLIN / EPOLLOUT）
        std::uint32_t m_readyEvents{0};  ///< 已上报但尚未被取走的就绪位（EPOLLIN / EPOLLOUT）
        WaiterSlot    m_readWaiter;      ///< 读方向等待者
        WaiterSlot    m_writeWaiter;     ///< 写方向等待者
    };

} // namespace AsynGyanis::Core
