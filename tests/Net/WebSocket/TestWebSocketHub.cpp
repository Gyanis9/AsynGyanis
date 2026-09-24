// WebSocketHub 的用例：扇出到全员、RAII 除名、单成员单写者的合并、队满丢新并计数、收口成员不再被碰。
// 这里用真的 WebSocketPeer，只把它的发送回调换成可停可放的记录槽——集线器管的是「谁在写、写多少、
// 什么时候不该再写」，那三件事都不需要真 sockets 就能钉死；线上字节由对端与帧层的用例各自守着。
//
// 「单写者闩」那条判据按突变法证过会转红（2026-09-24：把 publish 里的 isDraining 早退改成永不成立的条件，
// SecondPublishToABusyMember… 与 QueueBound… 两条同时红、复原后复绿）。
// 真会话里的嵌套写另有一条端到端用例守着：TestWebSocketSession.cpp 的 WebSocketHubFanout。
#include "Net/WebSocket/WebSocketHub.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Coroutine/Task.h"
#include "Net/WebSocket/WebSocketPeer.h"

#include "gtest/gtest.h"

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 一帧的上线字节里应当能直接看到负载文本：服务端发出的帧不带掩码，负载是原文
        [[nodiscard]] bool frameCarriesText(const std::string &frame, const std::string_view text)
        {
            return frame.find(text) != std::string::npos;
        }

        /**
         * @brief 可停可放的发送回调：既是记录槽，也是「让一帧停在挂起点」的闸门
         * @details 置 isGated 后，新的写出会停在 await_suspend 里并把句柄交回测试；测试据此能造出
         *          「某个成员正有一帧在写」这个状态，再观察第二路发布会不会另起一条写路径。
         */
        struct GatedSendPath
        {
            std::vector<std::string> sentFrames; ///< 已交出的帧字节，按完成顺序
            std::coroutine_handle<>  parkedWriter{}; ///< 停在闸门上的写协程句柄；空表示没人停着
            bool                     isGated{false}; ///< 是否让写出停在闸门

            /**
             * @brief WebSocketPeer 的 FrameSender 形状
             * @param bytes 已编码的一帧字节
             * @return Core::Task<bool> 恒为 true（本测试不造写失败）
             */
            Core::Task<bool> operator()(const std::string_view bytes)
            {
                if (isGated)
                {
                    co_await parkHere();
                }
                sentFrames.push_back(std::string(bytes));
                co_return true;
            }

            /**
             * @brief 放行并恢复停在闸门上的写协程
             * @return true 确实有一个写协程被恢复
             */
            bool release()
            {
                if (!parkedWriter)
                {
                    return false;
                }
                const std::coroutine_handle<> writer = std::exchange(parkedWriter, {});
                isGated                              = false;
                writer.resume();
                return true;
            }

        private:
            /**
             * @brief 把当前协程挂进 parkedWriter 的极简 awaitable
             */
            struct ParkHere
            {
                GatedSendPath &path;

                [[nodiscard]] bool await_ready() const noexcept
                {
                    return false;
                }

                void await_suspend(const std::coroutine_handle<> handle) noexcept
                {
                    path.parkedWriter = handle;
                }

                void await_resume() const noexcept {}
            };

            [[nodiscard]] ParkHere parkHere()
            {
                return ParkHere{*this};
            }
        };

        /// 驱动 publish：它只在闸门放开时才会真的跑完
        void drivePublish(Core::Task<void> task)
        {
            task.handle().resume();
            EXPECT_TRUE(task.isReady()) << "publish 没有同步跑完：写协程停在闸门上";
        }

        /**
         * @brief 造一条挂在给定发送回调上的对端的构造参数
         * @details 对端禁拷贝禁移动（它与所在会话的协程帧一对一），所以这里交出的不是对端本身，
         *          而是它的发送回调——用例里就地构造 `WebSocketPeer peer{makeFrameSender(path)};`
         * @param path 记录槽兼闸门；其生存期必须覆盖对端
         * @return WebSocketPeer::FrameSender 装配好的回调
         */
        WebSocketPeer::FrameSender makeFrameSender(GatedSendPath &path)
        {
            return [&path](const std::string_view bytes)
            {
                return path(bytes);
            };
        }
    } // namespace

    TEST(WebSocketHub, PublishReachesEveryMemberOfThatTopicOnly)
    {
        GatedSendPath lobbyFirst;
        GatedSendPath lobbySecond;
        GatedSendPath otherRoom;
        WebSocketPeer firstPeer{makeFrameSender(lobbyFirst)};
        WebSocketPeer secondPeer{makeFrameSender(lobbySecond)};
        WebSocketPeer otherPeer{makeFrameSender(otherRoom)};

        WebSocketHub hub;
        auto lobbyFirstSub  = hub.subscribe("lobby", firstPeer);
        auto lobbySecondSub = hub.subscribe("lobby", secondPeer);
        auto otherSub       = hub.subscribe("news", otherPeer);

        EXPECT_EQ(hub.memberCount("lobby"), 2U);
        EXPECT_EQ(hub.memberCount("news"), 1U);
        EXPECT_EQ(hub.subscriptionCount(), 3U);
        EXPECT_NE(lobbyFirstSub.id(), lobbySecondSub.id()) << "同主题两份订阅必须各自有号，除名才不会互相摘错";

        drivePublish(hub.publish("lobby", "hello room"));

        EXPECT_EQ(lobbyFirst.sentFrames.size(), 1U);
        EXPECT_EQ(lobbySecond.sentFrames.size(), 1U);
        EXPECT_TRUE(frameCarriesText(lobbyFirst.sentFrames[0], "hello room"));
        EXPECT_TRUE(frameCarriesText(lobbySecond.sentFrames[0], "hello room"));
        EXPECT_TRUE(otherRoom.sentFrames.empty()) << "别的主题的成员不该收到这条";
    }

    TEST(WebSocketHub, SubscriptionHandleRemovesTheMemberOnDestruction)
    {
        GatedSendPath path;
        WebSocketPeer peer{makeFrameSender(path)};
        WebSocketHub hub;

        {
            auto subscription = hub.subscribe("lobby", peer);
            EXPECT_TRUE(subscription.isActive());
            EXPECT_EQ(hub.subscriptionCount(), 1U);
            drivePublish(hub.publish("lobby", "one"));
        }

        EXPECT_EQ(hub.subscriptionCount(), 0U) << "句柄析构即除名：集线器不得留悬垂的对端指针";
        drivePublish(hub.publish("lobby", "two"));
        EXPECT_EQ(path.sentFrames.size(), 1U) << "除名之后还发得出消息，说明快照里那条成员没被认出来已走";
    }

    TEST(WebSocketHub, ResetAndMoveBothKeepExactlyOneUnsubscribe)
    {
        GatedSendPath path;
        WebSocketPeer peer{makeFrameSender(path)};
        WebSocketHub hub;

        auto original = hub.subscribe("lobby", peer);
        auto moved    = std::move(original);
        EXPECT_TRUE(moved.isActive());
        EXPECT_FALSE(original.isActive()) << "移走后原句柄必须变空，否则两次析构会摘掉别人的成员";
        EXPECT_EQ(hub.subscriptionCount(), 1U);

        // 空句柄上的除名是幂等的：析构与显式 reset 同时发生也不报错、不误删
        moved.reset();
        moved.reset();
        EXPECT_EQ(hub.subscriptionCount(), 0U);
        EXPECT_EQ(moved.id(), 0U);
    }

    TEST(WebSocketHub, SecondPublishToABusyMemberQueuesInsteadOfStartingAnotherWriter)
    {
        GatedSendPath path;
        path.isGated = true; // 第一帧停在闸门上，制造「这一帧正在写」
        WebSocketPeer peer{makeFrameSender(path)};
        WebSocketHub hub;
        auto subscription = hub.subscribe("lobby", peer);

        // 第一路 publish 会替成员写，并且正停在闸门上
        Core::Task<void> firstPublish = hub.publish("lobby", "alpha");
        firstPublish.handle().resume();
        ASSERT_TRUE(static_cast<bool>(path.parkedWriter)) << "闸门没起作用：第一帧根本没挂起";
        EXPECT_EQ(path.sentFrames.size(), 0U);

        // 第二路 publish 撞见同一个忙成员：只入队，绝不另起一条写路径（两帧字节会在线上互相穿插）
        drivePublish(hub.publish("lobby", "bravo"));
        EXPECT_EQ(path.sentFrames.size(), 0U);
        ASSERT_TRUE(static_cast<bool>(path.parkedWriter));

        path.release(); // 放行：drain 应当把队列里第二条一起带走
        firstPublish.handle().promise().result();

        ASSERT_EQ(path.sentFrames.size(), 2U) << "合并后应当按序发出两条";
        EXPECT_TRUE(frameCarriesText(path.sentFrames[0], "alpha"));
        EXPECT_TRUE(frameCarriesText(path.sentFrames[1], "bravo")) << "入队顺序就是上线顺序：慢读者不该看到插队";
    }

    TEST(WebSocketHub, QueueBoundDropsTheNewestMessageAndCountsIt)
    {
        constexpr std::size_t kPendingByteBound = 16U;
        GatedSendPath path;
        path.isGated = true;
        WebSocketPeer peer{makeFrameSender(path)};
        WebSocketHub hub(kPendingByteBound);
        auto subscription = hub.subscribe("lobby", peer);

        Core::Task<void> firstPublish = hub.publish("lobby", "0123456789abcdefghij"); // 20 字节，比整个上界还大
        firstPublish.handle().resume();
        EXPECT_EQ(hub.droppedMessageCount(), 1U) << "单条就超过上界的消息永远放不下：必须丢掉并计数，不能悄悄超编";

        // 第二路填进队列并替成员开写，随后停在闸门上：此刻它已出队，队列占用回到 0
        Core::Task<void> secondPublish = hub.publish("lobby", "1234567890");
        secondPublish.handle().resume();
        ASSERT_TRUE(static_cast<bool>(path.parkedWriter));

        // 第三路排进队列（10 ≤ 16）；第四路会让占用变成 20，越界的是它
        Core::Task<void> thirdPublish = hub.publish("lobby", "abcdefghij");
        thirdPublish.handle().resume();
        Core::Task<void> fourthPublish = hub.publish("lobby", "klmnopqrst");
        fourthPublish.handle().resume();
        EXPECT_EQ(hub.droppedMessageCount(), 2U) << "越界的应该是最后一条：丢最新而不打乱已入队的顺序";

        path.release();
        firstPublish.handle().promise().result();
        secondPublish.handle().promise().result();
        thirdPublish.handle().promise().result();
        fourthPublish.handle().promise().result();

        ASSERT_EQ(path.sentFrames.size(), 2U) << "在途那条与排进队列的那条该发出，越界的第三条不该出现";
        EXPECT_TRUE(frameCarriesText(path.sentFrames[0], "1234567890"));
        EXPECT_TRUE(frameCarriesText(path.sentFrames[1], "abcdefghij"));
        EXPECT_EQ(path.sentFrames[1].find("klmnopqrst"), std::string::npos);
    }

    TEST(WebSocketHub, ClosedPeerIsNotTouchedAndItsQueueIsEmptyd)
    {
        GatedSendPath path;
        WebSocketPeer peer{makeFrameSender(path)};
        peer.markClosed(); // 会话侧收口：此后 send* 一律不该再被调用

        WebSocketHub hub;
        auto subscription = hub.subscribe("lobby", peer);

        drivePublish(hub.publish("lobby", "after-close"));
        EXPECT_TRUE(path.sentFrames.empty()) << "对端已收口还去写它：单写者假设之外的字节没人负责";

        // 队列必须被排空而不是留着：一条收口的连接不该继续占内存
        drivePublish(hub.publish("lobby", "again"));
        EXPECT_TRUE(path.sentFrames.empty());
        EXPECT_EQ(hub.droppedMessageCount(), 0U) << "收口不是队满，不该记进丢弃计数";
    }

    TEST(WebSocketHub, RejectsZeroQueueBoundAtConstruction)
    {
        EXPECT_THROW(
                []
                {
                    const WebSocketHub zeroBound(0U);
                    static_cast<void>(zeroBound);
                }(),
                Base::InvalidArgumentException);
    }

    TEST(WebSocketHub, PublishWithNoMembersDoesNothingAndStaysSynchronous)
    {
        WebSocketHub hub;
        drivePublish(hub.publish("empty", "nobody here"));
        EXPECT_EQ(hub.subscriptionCount(), 0U);
    }
} // namespace AsynGyanis::Net
