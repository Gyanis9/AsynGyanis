// Connection 单元测试：构造、关闭、取消传播、地址文本与基类协程启动

#include "Core/Socket/Connection.h"

#include "Base/Exception/SystemException.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Socket/InetAddress.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace AsynGyanis::Core
{
    /**
     * @brief 构造后连接立即处于存活态并持有传入的 socket（描述符原样保留，不做替换）
     */
    TEST(Connection, ConstructionKeepsSocketAndAliveFlag)
    {
        EventLoop loop;
        Connection connection(AsyncSocket(loop, -1)); // 描述符 -1 的哑 socket，仅验证对象生命周期

        EXPECT_TRUE(connection.isAlive());
        EXPECT_EQ(connection.socket().fileDescriptor(), -1);
    }

    /**
     * @brief close() 把存活标志置为 false：后续查询据此拒绝继续读写的调用方
     */
    TEST(Connection, CloseMarksConnectionNotAlive)
    {
        EventLoop loop;
        Connection connection(AsyncSocket(loop, -1));

        connection.close();

        EXPECT_FALSE(connection.isAlive());
    }

    /**
     * @brief close() 同时向自身的取消对象广播停止请求：关闭即取消在途操作，不需调用方单独取消
     */
    TEST(Connection, CloseRequestsStopOnCancelable)
    {
        EventLoop loop;
        Connection connection(AsyncSocket(loop, -1));

        connection.close();

        EXPECT_TRUE(connection.cancelable().isStopRequested());
    }

    /**
     * @brief 基类 start() 不引入额外挂起点：单次 resume 即完成，不会吊住事件循环
     */
    TEST(Connection, BaseStartCompletesImmediately)
    {
        EventLoop loop;
        Connection connection(AsyncSocket(loop, -1));

        auto task = connection.start();
        task.handle().resume();
        EXPECT_TRUE(task.isReady());
    }

    /**
     * @brief 地址文本按「ip:端口」交出，取不到对端时如实抛错
     * @details 这两个方法是访问日志与观测指标里对端身份的唯一来源，也是清扫协程之外没人复查的
     *          字符串。两条判据各有其害：格式错了让日志解析与按端口归并失真；而「拿不到对端」若
     *          被折成 0.0.0.0:0 这类看起来合法的地址，故障连接就会在统计里冒充成一条合法对端。
     * @note 派生类（TLS 会话）必须重写这两个方法——基类读的是自己那条套接字的描述符，
     *       持有自有传输层的派生类不重写就会读到无效描述符上。
     */
    TEST(Connection, AddressTextIsIpPortAndUnconnectedPeerThrows)
    {
        EventLoop   loop;
        AsyncSocket socket = AsyncSocket::create(loop);
        ASSERT_TRUE(socket.bind(InetAddress::localhost(0)));
        const std::uint16_t boundPort = socket.localAddress().port();
        ASSERT_GT(boundPort, 0U) << "绑定后读不回端口，本用例的判据无从成立";

        Connection connection(std::move(socket));

        EXPECT_EQ(connection.localAddress(), "127.0.0.1:" + std::to_string(boundPort))
                << "本地地址文本与绑定值不一致：按端口归指标的日志会指着另一个监听器";
        // 取对端的动作要包一层：EXPECT_THROW 会丢掉返回值，而本方法是 [[nodiscard]] 的
        // （MSVC 据此报 C4834 并因「告警即错误」直接拒绝构建，GCC 不报这一条）
        const auto readPeerAddress = [&connection]() { return connection.remoteAddress(); };
        EXPECT_THROW(readPeerAddress(), Base::SystemException)
                << "只绑定未连接的套接字问不出对端，必须报错而不是交出「0.0.0.0:0」这类假地址";
    }

    /**
     * @brief 移动构造带走关闭状态：已关闭的源移动后，新对象仍是「不存活」
     */
    TEST(Connection, MoveConstructionPreservesAliveState)
    {
        EventLoop loop;
        Connection connection1(AsyncSocket(loop, -1));
        connection1.close();

        Connection connection2(std::move(connection1));

        EXPECT_FALSE(connection2.isAlive());
    }

    /**
     * @brief 移动赋值以源的状态覆盖目标：不会把一条已关闭的连接「复活」成存活
     */
    TEST(Connection, MoveAssignmentPreservesAliveState)
    {
        EventLoop loop;
        Connection connection1(AsyncSocket(loop, -1));
        Connection connection2(AsyncSocket(loop, -1));

        connection1.close();
        connection2 = std::move(connection1);

        EXPECT_FALSE(connection2.isAlive());
    }

    /**
     * @brief cancelable() 暴露内部真实取消对象（引用）：外部请求停止与其状态查询保持同步
     */
    TEST(Connection, CancelableReflectsStopRequest)
    {
        EventLoop loop;
        Connection connection(AsyncSocket(loop, -1));

        Cancelable &cancelable = connection.cancelable();
        ASSERT_FALSE(cancelable.isStopRequested());

        EXPECT_TRUE(cancelable.requestStop());
        EXPECT_TRUE(cancelable.isStopRequested());
    }

    /**
     * @brief 未设置截止时间的连接不参与空闲清扫：非 HTTP 会话不该被超时误伤
     */
    TEST(Connection, WithoutIdleDeadlineItNeverReportsExpired)
    {
        EventLoop  loop;
        Connection connection(AsyncSocket(loop, -1));

        // 构造后没有截止时间，任何「现在」都不算超期
        const auto farFuture = std::chrono::steady_clock::now() + std::chrono::hours(24);
        EXPECT_FALSE(connection.isIdleExpired(std::chrono::steady_clock::now()));
        EXPECT_FALSE(connection.isIdleExpired(farFuture));
    }

    /**
     * @brief refreshIdleDeadline() 按传入时限重新计时：到点前不超期，到点后才算超期
     */
    TEST(Connection, RefreshIdleDeadlineMarksExpiryAfterTimeout)
    {
        EventLoop  loop;
        Connection connection(AsyncSocket(loop, -1));

        connection.refreshIdleDeadline(std::chrono::milliseconds{50});

        const auto now = std::chrono::steady_clock::now();
        EXPECT_FALSE(connection.isIdleExpired(now)) << "刚刷新的截止时间不该立刻到期";
        EXPECT_TRUE(connection.isIdleExpired(now + std::chrono::milliseconds{60})) << "超过时限后应判定为超期";

        // 时限为 0 表示关闭本项保护：不是「立即到期」
        connection.refreshIdleDeadline(std::chrono::milliseconds{0});
        EXPECT_FALSE(connection.isIdleExpired(now + std::chrono::hours(1))) << "0 应当清除截止时间而不是设成立即到期";

        // 负数一律按清除处理，避免设出一个已经过去的截止时间
        connection.refreshIdleDeadline(std::chrono::milliseconds{-5});
        EXPECT_FALSE(connection.isIdleExpired(now + std::chrono::hours(1)));
    }

    /**
     * @brief clearIdleDeadline() 撤销超时约束：清扫协程此后不会再判定它超期
     */
    TEST(Connection, ClearIdleDeadlineRemovesExpiry)
    {
        EventLoop  loop;
        Connection connection(AsyncSocket(loop, -1));

        connection.refreshIdleDeadline(std::chrono::milliseconds{1});
        connection.clearIdleDeadline();

        EXPECT_FALSE(connection.isIdleExpired(std::chrono::steady_clock::now() + std::chrono::hours(1)));
    }

    /**
     * @brief 移动构造带走截止时间：移动后的新对象仍然受同一份超时约束
     */
    TEST(Connection, MoveConstructionPreservesIdleDeadline)
    {
        EventLoop  loop;
        Connection connection1(AsyncSocket(loop, -1));
        connection1.refreshIdleDeadline(std::chrono::milliseconds{1});

        Connection connection2(std::move(connection1));

        EXPECT_TRUE(connection2.isIdleExpired(std::chrono::steady_clock::now() + std::chrono::hours(1)))
                << "移动后截止时间丢失：这条连接会被空闲清扫漏掉";
    }
}
