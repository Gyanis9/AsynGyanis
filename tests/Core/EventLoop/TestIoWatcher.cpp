// IoWatcher 单元测试：注册一次、关注位按需武装、就绪缓存、关闭唤醒与等待者互斥
//
// 本文件直接驱动 IoWatcher，不引入事件循环线程：事件分发就是 EventLoop::run() 里那一步
// （取回事件后交给注册对象的 handleEvents），测试自己做同样的一步，因此时序完全确定。
//
// 钉住的契约：
// 1、描述符在构造时注册一次，等待时按方向武装关注位，等待结束（事件上报）即由内核
//    自动解除——因此空闲的注册对象不会让事件循环反复被唤醒（Windows 侧 wepoll 只有
//    水平触发，长期武装一个「总是就绪」的方向会让 epoll_wait 每次立刻返回）；
// 2、上报时就绪若没有协程在等，会被**记下来**，下一次等待立即完成且不再武装；
// 3、销毁注册对象会唤醒仍挂着的等待者并以「未就绪」结束它的等待，等待方因此能收尾
//    而不是永久挂起（关闭描述符本身不会唤醒 epoll 的等待者）；
// 4、同一方向的第二个等待者当场抛错，而不是静默让其中一个永远等不到；
// 5、注册对象销毁后会从「存活登记表」里单独摘掉（同批里剩下的事件不得再派发给它，
//    也不得牵连别的注册对象）。

#include "Core/EventLoop/IoWatcher.h"

#include "Base/Exception/LogicException.h"
#include "Base/Exception/SystemException.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Platform/IO/FileDescriptor.h"

#include "CoreTestSupport.h"

#include <gtest/gtest.h>

#include <coroutine>
#include <memory>
#include <system_error>

namespace AsynGyanis::Core
{
    namespace
    {
        using TestSupport::dispatchOnce;

        /// 等待结果：true 表示事件就绪，false 表示注册已失效
        using WaitOutcome = bool;

        /**
         * @brief 等待指定注册对象的可读事件
         * @param watcher 目标注册对象
         * @return Task<WaitOutcome> 惰性协程，co_await 后得到等待结果
         */
        Task<WaitOutcome> waitReadableOnce(IoWatcher &watcher)
        {
            co_return co_await watcher.waitReadable();
        }

        /// 单轮分发的时限（毫秒）：等待唤醒这类判据必须落在有限时间内出结果，
        /// 缺陷表现为「叫不醒」时要让它超时失败，而不是把测试作业挂在那里
        constexpr int kDispatchTimeoutMilliseconds = 2000;

        /**
         * @brief 向文件描述符写一个字节，使其对端变为可读
         * @param fileDescriptor 目标描述符
         */
        void makeReadable(const int fileDescriptor)
        {
            const char payload = 'x';
            [[maybe_unused]] auto _ = Platform::FileDescriptor::write(fileDescriptor, &payload, 1);
        }

        /**
         * @brief 读走描述符里已到的字节，让它重新回到「无数据可读」
         * @param fileDescriptor 目标描述符
         */
        void consumeReadable(const int fileDescriptor)
        {
            char     payload = 0;
            ssize_t  readCount = 0;
            do
            {
                readCount = Platform::FileDescriptor::read(fileDescriptor, &payload, 1);
            } while (readCount > 0);
        }

        /**
         * @brief 等自己被叫醒，然后关掉对面板那条连接、停住循环
         * @details 复刻「会话收口时顺手关掉另一条连接」的形状：恢复等待者就发生在这一批事件的
         *          派发过程中，同一批里后一条事件此时已指向一个死掉的对象。
         * @param own 本协程等待的注册对象
         * @param peer 要在自己醒来时销毁的注册对象（两条协程互为对方的 peer）
         * @param loop 事件循环（收尾时停住，run() 才会在测试线程上返回）
         * @return Task<WaitOutcome> 惰性协程；交回 true 表示是被事件叫醒的，false 表示对面板
         *         先收口、把自己销毁了
         */
        Task<WaitOutcome> wakeAndClosePeer(IoWatcher &own, std::unique_ptr<IoWatcher> &peer, EventLoop &loop)
        {
            const bool isReady = co_await own.waitReadable();
            peer.reset();
            loop.stop();
            co_return isReady;
        }
    } // namespace

    /**
     * @brief 销毁一个注册对象只会把它自己从存活登记表里摘掉
     * @details 派发前的闸门问的就是这张表。两个方向都各有其害：不摘，同批的后一条事件就被派到
     *          已释放的对象上（往死对象里写成员并 resume 垃圾句柄）；摘成「清空整张表」，剩下的
     *          连接再也收不到事件，表现为读写永久挂起。
     */
    TEST(IoWatcher, DestroyingOneWatcherOnlyRemovesThatOneFromTheAliveRegistry)
    {
        EventLoop loop;

        int firstLocal = -1;
        int firstPeer  = -1;
        int victimLocal = -1;
        int victimPeer  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(firstLocal, firstPeer));
        ASSERT_TRUE(Platform::FileDescriptor::createPair(victimLocal, victimPeer));

        const IoWatcher firstWatcher(loop, firstLocal);
        auto            victimWatcher = std::make_unique<IoWatcher>(loop, victimLocal);
        const IoWatcher *victimAddress = victimWatcher.get();
        ASSERT_TRUE(loop.isWatcherAlive(&firstWatcher)) << "构造时没登记，闸门会把所有事件都当成 stale";
        ASSERT_TRUE(loop.isWatcherAlive(victimAddress));

        victimWatcher.reset();

        EXPECT_FALSE(loop.isWatcherAlive(victimAddress))
                << "注册对象析构后还留在存活表里：同批事件会被派发到已释放对象上";
        EXPECT_TRUE(loop.isWatcherAlive(&firstWatcher))
                << "注销一个却带走了整张表：剩下的连接再也收不到事件";

        Platform::FileDescriptor::close(firstLocal);
        Platform::FileDescriptor::close(firstPeer);
        Platform::FileDescriptor::close(victimLocal);
        Platform::FileDescriptor::close(victimPeer);
    }

    /**
     * @brief 同一批里的后一条事件不得派发给已被前一条销毁的注册对象
     * @details 事件是从内核一批批取回来的：派发循环拿的是取回那一刻的快照，而先处理的那条完全
     *          可能销毁后一条所属的对象。这里刻意走 EventLoop::run() 而不是测试自己的分发步骤
     *          ——存活闸门只长在 run() 里，自己分发等于绕过被测代码。
     *          两条连接各挂一个等待者、醒来后互为对方去销毁：于是无论内核把哪条排在前面，
     *          后一条一定是「已经死了的那个」，本用例的前提不依赖批内顺序。
     * @note 判据是「两个结果必然一真一假」：被事件叫醒的那个交回 true，另一个只能是被对面板
     *       销毁时叫醒的（交回 false）。若哪天两条事件不再同批，两个都会是 true，本用例就会红——
     *       它不会静默地退化成一个不测任何东西的用例。
     * @note 摘掉派发前的 isWatcherAlive 判定后，本用例在 ASan 下报 heap-use-after-free
     */
    TEST(IoWatcher, StaleEventInSameBatchIsNotDispatchedToADestroyedWatcher)
    {
        EventLoop loop;

        int leftLocal = -1;
        int leftPeer  = -1;
        int rightLocal = -1;
        int rightPeer  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(leftLocal, leftPeer));
        ASSERT_TRUE(Platform::FileDescriptor::createPair(rightLocal, rightPeer));

        auto leftWatcher  = std::make_unique<IoWatcher>(loop, leftLocal);
        auto rightWatcher = std::make_unique<IoWatcher>(loop, rightLocal);
        ASSERT_TRUE(leftWatcher->isValid());
        ASSERT_TRUE(rightWatcher->isValid());

        // 先把构造时的一次性探测取走，再让两条都可读：两条等待一定都挂得住，
        // 而两次可读会在 run() 第一次取事件时一起回来
        static_cast<void>(dispatchOnce(loop, 0));
        makeReadable(leftPeer);
        makeReadable(rightPeer);

        Task<WaitOutcome> left  = wakeAndClosePeer(*leftWatcher, rightWatcher, loop);
        Task<WaitOutcome> right = wakeAndClosePeer(*rightWatcher, leftWatcher, loop);
        left.handle().resume();
        right.handle().resume();
        ASSERT_FALSE(left.isReady() || right.isReady()) << "两条等待都没挂住：本用例没测到派发途中恢复的形状";

        const IoWatcher *leftAddress  = leftWatcher.get();
        const IoWatcher *rightAddress = rightWatcher.get();

        // 由测试线程自己跑 run()：取事件、恢复、互相销毁全在一个线程上，时序完全确定
        loop.run();

        ASSERT_TRUE(left.isReady()) << "run() 返回了，左边的等待却没收尾";
        ASSERT_TRUE(right.isReady()) << "run() 返回了，右边的等待却没收尾";
        const bool leftWokenByEvent   = left.handle().promise().result();
        const bool rightWokenByEvent  = right.handle().promise().result();
        EXPECT_NE(leftWokenByEvent, rightWokenByEvent)
                << "两条都拿到事件或都没拿到：说明两次销毁没落在同一次派发里，闸门那条路径没被走到";

        EXPECT_FALSE(loop.isWatcherAlive(leftAddress)) << "注册对象销毁后仍留在存活表里";
        EXPECT_FALSE(loop.isWatcherAlive(rightAddress)) << "注册对象销毁后仍留在存活表里";

        Platform::FileDescriptor::close(leftLocal);
        Platform::FileDescriptor::close(leftPeer);
        Platform::FileDescriptor::close(rightLocal);
        Platform::FileDescriptor::close(rightPeer);
    }

#if ASYN_PLATFORM_WIN32
    /**
     * @brief 已经挂在别的完成端口上的描述符换一条循环注册不上，而且报错要带得出原因
     * @details 句柄与完成端口的关联是**内核对象级**且不可解除的：这正是「把监听套接字交给另一个进程」
     *          在 Windows 上走不通的那条硬限制——新一代的事件循环接不上那份句柄（POSIX 上同一个描述符
     *          能挂在两个 epoll 实例下，因此这条只在 Windows 侧断言）。
     *          断言落在错误码与它的空间上：这一句拒绝来自 kernel32，码在 GetLastError 空间里；抛出方
     *          若按默认的 errno 通道取码，日志上就是「[0] success」——看着像根本没失败过，白查一轮。
     */
    TEST(IoWatcher, DescriptorOnAnotherCompletionPortThrowsWithWin32Reason)
    {
        EventLoop firstLoop;
        EventLoop secondLoop;
        int       local = -1;
        int       peer  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(local, peer));

        bool            isThrown = false;
        std::error_code captured;
        {
            const IoWatcher firstWatcher(firstLoop, local);
            try
            {
                const IoWatcher secondWatcher(secondLoop, local);
            }
            catch (const Base::SystemException &error)
            {
                isThrown = true;
                captured = error.errorCode();
            }
        }
        Platform::FileDescriptor::close(local);
        Platform::FileDescriptor::close(peer);

        EXPECT_TRUE(isThrown) << "换一条循环居然注册上了：完成端口的关联不是句柄级的，示例与文档的结论都要改";
        EXPECT_NE(captured.value(), 0) << "注册被拒却没有带出原因码";
        EXPECT_TRUE(captured.category() == std::system_category())
                << "码取自 GetLastError 却按 errno 空间解释：报出来的描述与本次失败无关";
    }
#endif

    TEST(IoWatcher, InvalidDescriptorYieldsUnusableWatcher)
    {
        EventLoop loop;

        // 负数描述符（占位、已关闭）不注册，也不抛异常：持有空描述符的对象因此可以统一处理
        const IoWatcher watcher(loop, -1);
        EXPECT_FALSE(watcher.isValid());

        // 在这种对象上等待会立刻以「未就绪」结束，而不是挂起或抛异常
        Task<WaitOutcome> waiting = waitReadableOnce(const_cast<IoWatcher &>(watcher));
        waiting.handle().resume();
        ASSERT_TRUE(waiting.isReady());
        EXPECT_FALSE(waiting.handle().promise().result());
    }

    /**
     * @brief 没有任何等待者时，注册对象不得让事件反复上报（水平触发下的空转防线）
     *
     * @details Windows 侧的 wepoll 没有边沿触发，关注位一旦长期武装，只要那个方向「为真」
     *          （这里是「对端已写入数据」这种持续可读状态），每次 epoll_wait 都会立刻返回它。
     *          本用例先造出这个持续为真的状态，再确认空闲的注册对象**不产生任何事件**——
     *          若谁把关注位改回常驻武装，这里会立刻失败。
     */
    TEST(IoWatcher, IdleRegistrationKeepsProducingNoEvents)
    {
        EventLoop loop;

        int localDescriptor = -1;
        int peerDescriptor  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        IoWatcher watcher(loop, localDescriptor);
        ASSERT_TRUE(watcher.isValid());

        // 对端写入后，localDescriptor 进入「持续可读」状态；构造时的一次性探测最多被消耗一次
        makeReadable(peerDescriptor);
        [[maybe_unused]] const std::size_t probeEventCount = dispatchOnce(loop, 0);

        // 之后没有等待者，就一次事件都不该再有——有的话就是空转
        EXPECT_EQ(dispatchOnce(loop, 0), 0U) << "空闲的注册对象仍在产生事件：事件循环会被反复空唤醒";
        EXPECT_EQ(dispatchOnce(loop, 0), 0U) << "空闲的注册对象仍在产生事件：事件循环会被反复空唤醒";

        Platform::FileDescriptor::close(localDescriptor);
        Platform::FileDescriptor::close(peerDescriptor);
    }

    TEST(IoWatcher, WaitCompletesWhenEventIsDispatched)
    {
        EventLoop loop;

        int localDescriptor = -1;
        int peerDescriptor  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        IoWatcher watcher(loop, localDescriptor);
        ASSERT_TRUE(watcher.isValid());

        Task<WaitOutcome> waiting = waitReadableOnce(watcher);
        waiting.handle().resume();
        ASSERT_FALSE(waiting.isReady()) << "尚无数据时应当挂起等待";

        // 等待期间关注位已武装：让事件到达并分发，等待就完成了
        makeReadable(peerDescriptor);
        ASSERT_GT(dispatchOnce(loop), 0U);

        ASSERT_TRUE(waiting.isReady());
        EXPECT_TRUE(waiting.handle().promise().result());

        Platform::FileDescriptor::close(localDescriptor);
        Platform::FileDescriptor::close(peerDescriptor);
    }

    /**
     * @brief 钉住 isWaitingFor 的口径：挂起期间为真，完成后为假
     * @details TLS 侧靠它判断「本方向需要反方向先推进时能不能去等对方方向」——一个方向只允许
     *          一个等待者，抢槽会抛异常，所以先问一句再决定走哪条让出路径
     */
    TEST(IoWatcher, ReportsWhetherADirectionHasAWaiter)
    {
        EventLoop loop;

        int localDescriptor = -1;
        int peerDescriptor  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        IoWatcher watcher(loop, localDescriptor);
        ASSERT_TRUE(watcher.isValid());

        EXPECT_FALSE(watcher.isWaitingFor(EPOLLIN)) << "还没人等待时应当为假";
        EXPECT_FALSE(watcher.isWaitingFor(EPOLLOUT));

        Task<WaitOutcome> waiting = waitReadableOnce(watcher);
        waiting.handle().resume();
        ASSERT_FALSE(waiting.isReady());
        EXPECT_TRUE(watcher.isWaitingFor(EPOLLIN)) << "读方向已有等待者";
        EXPECT_FALSE(watcher.isWaitingFor(EPOLLOUT)) << "写方向不受影响";

        makeReadable(peerDescriptor);
        ASSERT_GT(dispatchOnce(loop), 0U);
        ASSERT_TRUE(waiting.isReady());
        EXPECT_FALSE(watcher.isWaitingFor(EPOLLIN)) << "等待完成后不该再报有等待者";

        Platform::FileDescriptor::close(localDescriptor);
        Platform::FileDescriptor::close(peerDescriptor);
    }

    TEST(IoWatcher, ReadinessIsCachedWhenNobodyIsWaiting)
    {
        EventLoop loop;

        int localDescriptor = -1;
        int peerDescriptor  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        IoWatcher watcher(loop, localDescriptor);
        ASSERT_TRUE(watcher.isValid());

        // 第一轮等待把关注位武装起来，随后在等待者已消失（协程帧被销毁）的情况下让事件到达：
        // 这一份上报没人领，必须被记下来，否则等待方会一直睡下去
        {
            Task<WaitOutcome> abandoned = waitReadableOnce(watcher);
            abandoned.handle().resume();
            ASSERT_FALSE(abandoned.isReady());
        }
        ASSERT_TRUE(watcher.isValid());

        makeReadable(peerDescriptor);
        ASSERT_GT(dispatchOnce(loop), 0U);

        // 之后才发起的等待应当立即完成（不再需要任何新事件，也不必重新武装）
        Task<WaitOutcome> waiting = waitReadableOnce(watcher);
        waiting.handle().resume();
        ASSERT_TRUE(waiting.isReady()) << "上报时就绪没有被下一次等待取走";
        EXPECT_TRUE(waiting.handle().promise().result());

        // 取走之后标记不再残留：再等一次会重新挂起（否则就绪标记会让等待空转）
        Task<WaitOutcome> secondWaiting = waitReadableOnce(watcher);
        secondWaiting.handle().resume();
        EXPECT_FALSE(secondWaiting.isReady()) << "就绪标记被取走后不应当再次立即完成";

        Platform::FileDescriptor::close(localDescriptor);
        Platform::FileDescriptor::close(peerDescriptor);
    }

    TEST(IoWatcher, DestroyingWatcherWakesPendingWaiter)
    {
        EventLoop loop;

        int localDescriptor = -1;
        int peerDescriptor  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        auto watcher = std::make_unique<IoWatcher>(loop, localDescriptor);
        ASSERT_TRUE(watcher->isValid());

        Task<WaitOutcome> waiting = waitReadableOnce(*watcher);
        waiting.handle().resume();
        ASSERT_FALSE(waiting.isReady());

        // 销毁注册对象（等价于关闭描述符那条路径）：仍挂着的等待者必须被唤醒并得知注册已失效，
        // 否则它会永远等一个不可能再到来的事件
        watcher.reset();

        // 唤醒是投递到调度队列的（本对象正在析构，不能就地恢复）。
        // 队列里通常还压着循环自带的常驻协程（定时器队列的驱动），因此推进到清空为止
        loop.scheduler().runAll();
        ASSERT_TRUE(waiting.isReady());
        EXPECT_FALSE(waiting.handle().promise().result());

        Platform::FileDescriptor::close(localDescriptor);
        Platform::FileDescriptor::close(peerDescriptor);
    }

    TEST(IoWatcher, SecondWaiterOnSameDirectionThrows)
    {
        EventLoop loop;

        int localDescriptor = -1;
        int peerDescriptor  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        IoWatcher watcher(loop, localDescriptor);
        ASSERT_TRUE(watcher.isValid());

        Task<WaitOutcome> first = waitReadableOnce(watcher);
        first.handle().resume();
        ASSERT_FALSE(first.isReady());

        // 同一方向的第二个等待者：当场抛错而不是让其中一个永远等不到
        Task<WaitOutcome> second = waitReadableOnce(watcher);
        second.handle().resume();
        ASSERT_TRUE(second.isReady()) << "第二个等待者应当以异常结束";
        EXPECT_THROW(second.handle().promise().result(), Base::LogicException);

        Platform::FileDescriptor::close(localDescriptor);
        Platform::FileDescriptor::close(peerDescriptor);
    }

    /**
     * @brief 钉住头文件的自述「注册一次即可反复等待」：第二轮等待必须由**新**到的事件唤醒
     * @details IOCP 后端的一条读探针只上报一次，第二轮能否被唤醒取决于「上报之后在下一轮 wait()
     *          前重新投探针」这条水平触发等价路径是否真走到。用例先把上一轮的字节取干净，
     *          使第二轮只能靠新事件醒来；超时落在有限值上，叫不醒就判失败。
     */
    TEST(IoWatcher, SecondWaitIsAwokenByTheNextEvent)
    {
        EventLoop loop;

        int localDescriptor = -1;
        int peerDescriptor  = -1;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, peerDescriptor));

        IoWatcher watcher(loop, localDescriptor);
        ASSERT_TRUE(watcher.isValid());

        // ---- 第一轮：数据先到，分发一次即唤醒 ----
        makeReadable(peerDescriptor);
        Task<WaitOutcome> firstWait = waitReadableOnce(watcher);
        firstWait.handle().resume();
        ASSERT_FALSE(firstWait.isReady()) << "尚未分发时第一轮应当挂起";
        ASSERT_GT(dispatchOnce(loop, kDispatchTimeoutMilliseconds), 0U) << "第一轮等待没被唤醒";
        ASSERT_TRUE(firstWait.isReady());
        EXPECT_TRUE(firstWait.handle().promise().result());

        // 取走字节：第二轮不得靠残留的可读状态立即完成
        consumeReadable(localDescriptor);

        // ---- 第二轮：同一个常驻注册对象上再等一次 ----
        Task<WaitOutcome> secondWait = waitReadableOnce(watcher);
        secondWait.handle().resume();
        ASSERT_FALSE(secondWait.isReady()) << "描述符已空时第二轮应当挂起";

        makeReadable(peerDescriptor);
        EXPECT_GT(dispatchOnce(loop, kDispatchTimeoutMilliseconds), 0U)
            << "第二轮等待没被新事件唤醒：常驻注册的关注位没有重新武装，「注册一次即可反复等待」不成立";
        EXPECT_TRUE(secondWait.isReady()) << "第二轮等待没被新事件唤醒";

        Platform::FileDescriptor::close(localDescriptor);
        Platform::FileDescriptor::close(peerDescriptor);
    }

} // namespace AsynGyanis::Core
