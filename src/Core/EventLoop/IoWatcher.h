/**
 * @file IoWatcher.h
 * @brief 常驻 epoll 注册：注册一次即可反复等待，关注位只在有等待者时武装
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <optional>
#include <stop_token>

namespace AsynGyanis::Core
{
    class EventLoop;

    /**
     * @brief 一个文件描述符的常驻 epoll 注册与等待入口
     *
     * @note 线程约束：**包括析构在内，本类的所有方法都只在所属事件循环线程上调用**——析构会连带
     *       改事件后端的常驻注册表；需要从外部线程停止某个套接字/服务器时，把动作投递到那个循环
     *       （见 `TcpServer::close()` 的 @warning 与 `EventLoop::scheduler().scheduleRemote()`），
     *       不要直接调 close()/析构。
     * @note 一个方向同时只允许一个等待者：并发等待会抛 `Base::LogicException`
     *       （这几乎总是「读协程起了两个」这类用法错误，静默让某一个永远等不到危险得多）。
     * @warning 除上述投递方式外，不要从其它线程调用本类的方法。
     * @details 关注位按「谁在等」武装，采**水平触发 + 按需摘除**：无人等的方向必须显式收掉内核
     *          掩码——可写几乎长期为真，无人领就会持续上报把 epoll_wait 拖成空转；Windows 侧
     *          由「下一次 wait() 前对仍在掩码里的方向重投探针」等价实现。析构用 scheduleRemote()
     *          唤醒等待者：本对象正在析构，就地恢复会让协程在析构完成前回来访问成员（释放后使用）。
     */
    class ASYN_CORE_API IoWatcher
    {
    public:
        /**
         * @brief 等待器：与其它 awaitable 一样直接 co_await
         */
        class ASYN_CORE_API Awaiter
        {
        public:
            /**
             * @brief 构造等待器
             * @param watcher 目标注册对象
             * @param event 关注的事件位（EPOLLIN 或 EPOLLOUT）
             * @param stopToken 协作取消令牌，可为空；非空且可停止时，requestStop() 会把挂起中的
             *                  本次等待唤醒并以「被取消」结束（见 isCancellationRequested）
             */
            Awaiter(IoWatcher &watcher, std::uint32_t event, std::stop_token stopToken = {}) noexcept;

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
             * @return false 不挂起、立即收尾：注册已失效（描述符已关闭或武装失败，按「未就绪」结束），
             *         或令牌在挂起前已停止（按「被取消」结束）
             * @throws Base::LogicException 该方向已有另一个等待者
             */
            [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle);

            /**
             * @brief 取回等待结果
             * @return true 事件已就绪（调用方应立刻重试系统调用）
             * @return false 未就绪：注册已失效（描述符已关闭），或本次等待被取消唤醒（令牌被
             *         requestStop，见 isCancellationRequested() 区分两者）。两种情形调用方
             *         都应停止重试并收尾
             */
            [[nodiscard]] bool await_resume() const noexcept;

            /**
             * @brief 本次等待是否被取消唤醒（令牌被 requestStop）
             * @return true 取消已请求且本等待因此结束；false 其余情形
             * @note 线程安全：停止回调可能来自任意线程，本读数按原子量取
             */
            [[nodiscard]] bool isCancellationRequested() const noexcept;

        private:
            friend class IoWatcher;

            /**
             * @brief 由注册对象标记本次等待已就绪
             */
            void markReady() noexcept;

            /**
             * @brief 由注册对象（或挂起前的自检）标记本次等待被取消
             */
            void markCancelled() noexcept;

            /**
             * @brief 由注册对象告知「你已不在我的登记槽里」
             * @details 注册对象一旦把等待者从槽里取走（无论是为了恢复它，还是它自己在析构），
             *          都必须调用本方法。否则等待器析构时会以为自己还挂在注册对象上，
             *          去调用可能是**已被销毁**的注册对象——即释放后使用。
             */
            void markDetached() noexcept;

            /**
             * @brief 停止回调的落点：可能运行在任意线程（谁调 requestStop 就谁跑）
             * @details 只做两件线程安全的事——发布取消标记（原子量），再把「到循环线程上核一核
             *          这一代等待还在不在」投递回所属循环。不在此处碰注册对象与等待槽：那是循环
             *          线程的账（见 IoWatcher::cancelWaiterIfMatches），投递侧先经存活表判过再动手
             */
            void onStopRequested() noexcept;

            /// 停止回调的载体：std::stop_callback 要求可调用体类型固定，值语义的一层壳最省
            struct StopWake
            {
                Awaiter *awaiter; ///< 所属等待器；回调调用期它必然活着（~stop_callback 会等在建回调跑完）
                void     operator()() const noexcept
                {
                    awaiter->onStopRequested();
                }
            };

            IoWatcher                                  *m_watcher;                ///< 目标注册对象（非拥有）
            EventLoop                                  *m_loop;                   ///< 所属事件循环（取自注册对象；取消投递要用）
            std::uint32_t                               m_event;                  ///< 本次关注的事件位
            std::stop_token                             m_stopToken;              ///< 协作取消令牌；默认构造 = 不接取消唤醒
            std::uint64_t                               m_attachGeneration{0};    ///< 登记时的代次号：取消投递据此确认「还是那一次等待」
            bool                                        m_isAttached{false};      ///< 是否已登记到注册对象上
            bool                                        m_isReady{false};         ///< 结果：事件是否就绪
            std::atomic<bool>                           m_cancelRequested{false}; ///< 结果：是否被取消唤醒（回调可能来自任意线程）
            std::optional<std::stop_callback<StopWake>> m_stopCallback;           ///< 停止回调：挂起时注册，随等待器析构注销
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
         * @param stopToken 协作取消令牌，可为空；非空且可停止时，requestStop() 把挂起中的等待
         *                  唤醒并以「被取消」结束（消费方据 Awaiter::isCancellationRequested() 收口）
         * @return Awaiter 等待器；已就绪时 co_await 立即返回
         */
        [[nodiscard]] Awaiter waitReadable(std::stop_token stopToken = {}) noexcept;

        /**
         * @brief 等待该描述符可写（EPOLLOUT）
         * @param stopToken 语义同 waitReadable()
         * @return Awaiter 等待器；已就绪时 co_await 立即返回
         */
        [[nodiscard]] Awaiter waitWritable(std::stop_token stopToken = {}) noexcept;

        /**
         * @brief 该方向上是否已有协程在等待
         * @details 供「本方向需要反方向先推进」的情形判断：一个方向只允许一个等待者，
         *          抢槽会直接抛 LogicException，调用方应当据此改走别的让出方式
         * @param event 事件位（EPOLLIN 或 EPOLLOUT）
         * @return true 该方向已有等待者
         */
        [[nodiscard]] bool isWaitingFor(std::uint32_t event) const noexcept;

        /**
         * @brief 处理来自事件循环的事件分发
         * @param events epoll 报告的事件位（含错误与挂断位）
         * @note 恢复协程之后**不再访问任何成员**：被恢复的代码可能立刻销毁本对象
         *       （例如读到对端关闭后关闭连接），那之后访问成员就是释放后使用。
         * @details 仅由 EventLoop 在分发时调用：无人等待的方向把就绪记下来留给下一次等待，
         *          仍有人在等的方向在这里补一次武装（本次上报已消耗掉上一次的一次性关注）。
         */
        void handleEvents(std::uint32_t events) noexcept;

    private:
        /// 唤醒等待者的三种结局：就绪置位、未就绪不置位、取消置位（见 takeWaiter）
        enum class WakeReason
        {
            Ready,     ///< 事件就绪
            Closed,    ///< 注册失效（描述符已关闭或武装失败）
            Cancelled, ///< 令牌被停止
        };

        /**
         * @brief 一个方向上的等待者登记
         */
        struct WaiterSlot
        {
            std::coroutine_handle<> handle{};         ///< 等待中的协程，空表示当前无人等待
            Awaiter                *awaiter{nullptr}; ///< 对应的等待器，用于把结果写回它
            std::uint64_t           generation{0};    ///< 本槽位每次登记的代次号；取消投递据此拒掉迟到者（协程帧地址会被复用，指针相等分不开两代）
        };

        /// 「内核掩码未知」的哨兵：mod 失败时账本记它，保证下一次 armEvents 必定再下发一次 mod。
        /// 有效事件位只占低位（EPOLLIN/EPOLLOUT），与全 1 不可能相等
        static constexpr std::uint32_t kUnknownArmedEvents = ~static_cast<std::uint32_t>(0);

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
         * @brief 把某个方向的等待者从登记槽摘下，并按结局通知它
         * @param event 事件位（EPOLLIN 或 EPOLLOUT）
         * @param reason 唤醒结局：Ready 置就绪位、Closed 不置位、Cancelled 置取消位
         * @return 需要恢复的协程句柄，无人等待时为空
         */
        [[nodiscard]] std::coroutine_handle<> takeWaiter(std::uint32_t event, WakeReason reason) noexcept;

        /**
         * @brief 若该方向的等待者正是「那一代」的那一次等待，就地把取消外发出去
         * @details 由取消投递在循环线程上调用（投递侧已用存活表确认本对象还在）。代次号对不上
         *          说明那次等待早已结束（事件、关闭或又挂上了新一代），迟到的取消不许打在新等待者
         *          身上——协程帧的地址会被复用，单靠指针相等分不开两代
         * @param event 事件位（EPOLLIN 或 EPOLLOUT）
         * @param generation 登记时的代次号
         */
        void cancelWaiterIfMatches(std::uint32_t event, std::uint64_t generation) noexcept;

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

        EventLoop    *m_loop;                ///< 所属事件循环（非拥有）
        int           m_fileDescriptor;      ///< 被注册的文件描述符
        bool          m_isRegistered{false}; ///< 是否已成功注册到 epoll
        std::uint32_t m_armedEvents{0};      ///< 当前在核心里武装着的事件位（EPOLLIN / EPOLLOUT）
        std::uint32_t m_readyEvents{0};      ///< 已上报但尚未被取走的就绪位（EPOLLIN / EPOLLOUT）
        std::uint64_t m_attachGeneration{0}; ///< 等待者登记的代次号计数器：每次登记自增，取消投递据此认代
        WaiterSlot    m_readWaiter;          ///< 读方向等待者
        WaiterSlot    m_writeWaiter;         ///< 写方向等待者
    };

} // namespace AsynGyanis::Core
