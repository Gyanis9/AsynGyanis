/**
 * @file HttpOutboundEstablishment.h
 * @brief 出站连接池的「同一端点同时只建一次连」记账：领导者建连，后来者等它，醒来直接复用
 */

#ifndef ASYN_HTTP_OUTBOUND_ESTABLISHMENT_H
#define ASYN_HTTP_OUTBOUND_ESTABLISHMENT_H

#include <coroutine>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace AsynGyanis::Core
{
    class EventLoop;
} // namespace AsynGyanis::Core

namespace AsynGyanis::Net
{
    /**
     * @brief 每个端点的「建连在途」记账：连接池与等连的协程共用一份
     *
     * @details 冷池上同时进来的请求原本各自握手：5 条并发就是 5 遍 TCP+TLS+ALPN+h2 前奏，而 HTTP/2
     *          的意义正是一条连接上多路复用。规则改成「同一端点同时只允许一次建连」：第一个到的当
     *          领导者，后来的挂起等它；领导者结算时先让结论进池、再唤醒等待者，于是醒来就能复用。
     *
     * @note 唤醒只把协程投回它自己那条循环的调度器（走线程安全的那条口），本表不碰任何套接字。
     * @note 表由 `shared_ptr` 持有，等待者也拿一份：池先于某个挂着的等待者销毁时，后者析构要摸的是
     *       自己手里这张表，不是已经死掉的池（连接池踩过一次这个坑，代价是一起真实 UAF）。
     */
    class HttpEstablishmentTable
    {
    public:
        /// 挂在某个端点上的一次等待：节点由等待者的协程帧持有，摘链只动节点自己
        struct Waiter
        {
            std::coroutine_handle<> handle;            ///< 要唤醒的协程
            Core::EventLoop        *loop{};            ///< 它所属的事件循环（唤醒要投回这条线程）
            Waiter                 *previous{nullptr}; ///< 环形链表前驱；与 next 同时为空表示没挂着
            Waiter                 *next{nullptr};     ///< 环形链表后继
        };

        HttpEstablishmentTable() = default;

        HttpEstablishmentTable(const HttpEstablishmentTable &)            = delete;
        HttpEstablishmentTable &operator=(const HttpEstablishmentTable &) = delete;

        /**
         * @brief 问一句：这个端点有人在建连吗？没有就把领导者名额占下来
         * @param endpointKey 目标身份（与池里用的端点键文本一致）
         * @return true 本次成为领导者：去建连，结束时**必须** settle()
         * @return false 已有人在建：该 attach() 等它
         */
        bool tryBecomeLeader(const std::string &endpointKey);

        /**
         * @brief 把一次等待挂进某个端点的链表
         * @param endpointKey 目标身份
         * @param waiter 调用方（协程帧）持有的节点，之后必须 detach()
         * @return true 已挂上（等 settle 唤醒）
         * @return false 这个端点已经没有在途建连了（领导者刚好结算完）：调用方直接往下走，
         *         不要挂着一个没人会唤醒的节点
         */
        bool attach(const std::string &endpointKey, Waiter &waiter) noexcept;

        /**
         * @brief 把一次等待从链表上摘下来（带锁，供等待者那一侧调用）
         * @param waiter 之前 attach 过的节点；没挂着时是空操作
         * @details 被唤醒、协程帧被销毁、异常展开三条路都走这里；摘链只动节点自己，
         *          因此「谁先看到它」不重要——settle 唤醒之前也先摘。
         */
        void detach(Waiter &waiter) noexcept;

        /**
         * @brief 结算这个端点的建连：撤掉在途标记并唤醒全部等待者
         * @param endpointKey 目标身份
         * @details 不论建连成功还是失败都要调（成功时结论已经先进池，失败时等待者自己去建）。
         *          漏掉这一步等于把这个端点的出站请求永久堵死，所以调用方用 RAII 兜住。
         */
        void settle(const std::string &endpointKey);

    private:
        /// 一个端点的在途记录：哨兵自指的环形链表，「有人在建连」由这张表里有没有这一键表示
        struct Endpoint
        {
            /// 哨兵节点只当链表的头尾用，它的 handle 与 loop 永远不被读
            Waiter sentinel{};

            /// 哨兵自环：新建时它就是唯一的节点
            Endpoint() noexcept
            {
                sentinel.next     = &sentinel;
                sentinel.previous = &sentinel;
            }

            // 节点里存着指向自己成员的指针，拷贝会把链指到别的对象上，因此禁掉
            Endpoint(const Endpoint &)            = delete;
            Endpoint &operator=(const Endpoint &) = delete;
        };

        std::mutex                                m_mutex;     ///< 保护下面这张表
        std::unordered_map<std::string, Endpoint> m_endpoints; ///< 在建连的端点 → 等待链表

        /// 不带锁的摘链，只给 attach/detach/settle 这些已经持锁的地方用
        static void unlink(Waiter &waiter) noexcept;
    };

    /**
     * @brief 「等这个端点的建连结算」的可等待对象：`co_await` 它，醒来时结论已在池里
     *
     * @details 表按 `shared_ptr` 拿在手里（构造参数），所以即使连接池已经销毁，析构也只是往自己
     *          手里那张表还一次锁，不会摸到已释放的对象。
     * @note 等待本身不设时限：领导者那条建连是被它自己的请求时限管着的，它一结算这里就醒。
     *       醒来之后调用方仍要自己算剩余预算——这一段等待可能已经吃掉了它的一部分。
     */
    class HttpEstablishmentAwait
    {
    public:
        /**
         * @brief 组一个等待
         * @param table 记账表（与池共用一份，构造即持有引用）
         * @param endpointKey 等哪个端点的建连
         * @param loop 本协程跑在哪条循环上（决定唤醒投回哪里）
         */
        HttpEstablishmentAwait(std::shared_ptr<HttpEstablishmentTable> table, std::string endpointKey, Core::EventLoop &loop) noexcept;

        HttpEstablishmentAwait(const HttpEstablishmentAwait &)            = delete;
        HttpEstablishmentAwait &operator=(const HttpEstablishmentAwait &) = delete;

        /// 析构：协程帧被销毁而没等到唤醒时，把自己从链表上摘掉
        ~HttpEstablishmentAwait() noexcept;

        /// 已经不用等（端点在挂上之前就结算完了）时为真
        [[nodiscard]] bool await_ready() const noexcept;

        /**
         * @brief 挂进链表；挂不上就直接继续跑
         * @param handle 本协程
         * @return true 已挂起，等 settle 唤醒
         * @return false 无需挂起（调用方立刻往下走）
         */
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept;

        /// 醒来：没有返回值——结论在池里，调用方自己去取
        void await_resume() const noexcept;

    private:
        std::shared_ptr<HttpEstablishmentTable> m_table;       ///< 记账表，析构时要往它还一次锁
        std::string                             m_endpointKey; ///< 等的是哪个端点
        Core::EventLoop                        &m_loop;        ///< 本协程所属的循环
        HttpEstablishmentTable::Waiter          m_waiter;      ///< 链表节点（句柄在 suspend 时填）
    };
} // namespace AsynGyanis::Net

#endif // ASYN_HTTP_OUTBOUND_ESTABLISHMENT_H
