// WebSocketHub 的用例：扇出到全员、RAII 除名、单成员单写者的合并、队满丢新并计数、没人收的整队另记一本、
// 写路径抛出不把成员写聋、收口成员不再被碰。
// 这里用真的 WebSocketPeer，只把它的发送回调换成可停可放的记录槽——集线器管的是「谁在写、写多少、
// 什么时候不该再写」，那三件事都不需要真 sockets 就能钉死；线上字节由对端与帧层的用例各自守着。
//
// 「单写者闩」那条判据按突变法证过会转红（2026-09-24：把 publish 里的 isDraining 早退改成永不成立的条件，
// SecondPublishToABusyMember… 与 QueueBound… 两条同时红、复原后复绿）。
// 真会话里的嵌套写另有一条端到端用例守着：TestWebSocketSession.cpp 的 WebSocketHubFanout。
#include "Net/WebSocket/WebSocketHub.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Coroutine/Task.h"
#include "MetricsTestSupport.h"
#include "Net/WebSocket/WebSocketPeer.h"

#include "gtest/gtest.h"

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
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
         * @brief 取一帧首字节里的操作码位（低 4 位）
         * @details 服务端发出的帧不带掩码，首字节就是 FIN + 保留位 + 操作码，因此这一位直接区分
         *          「按文本帧发的」与「按二进制帧发的」——扇出的类型判据只能落在这里，负载文本本身看不出来
         * @param frame 已交出的整帧字节
         * @return std::uint8_t 操作码
         */
        [[nodiscard]] std::uint8_t frameOpCode(const std::string &frame)
        {
            return static_cast<std::uint8_t>(static_cast<std::uint8_t>(frame.at(0)) & 0x0FU);
        }

        /**
         * @brief 可停可放的发送回调：既是记录槽，也是「让一帧停在挂起点」的闸门
         * @details 置 isGated 后，新的写出会停在 await_suspend 里并把句柄交回测试；测试据此能造出
         *          「某个成员正有一帧在写」这个状态，再观察第二路发布会不会另起一条写路径。
         */
        struct GatedSendPath
        {
            std::vector<std::string> sentFrames;             ///< 已交出的帧字节，按完成顺序
            std::coroutine_handle<>  parkedWriter{};         ///< 停在闸门上的写协程句柄；空表示没人停着
            bool                     isGated{false};         ///< 是否让写出停在闸门
            bool                     isRefusing{false};      ///< 放开闸门后是否交回 false：造一次传输失败
            bool                     throwOnNextSend{false}; ///< 下一次写出是否抛出：造一次业务写回调的异常展开

            /**
             * @brief WebSocketPeer 的 FrameSender 形状
             * @param bytes 已编码的一帧字节
             * @return Core::Task<bool> 默认 true，置 isRefusing 后交回 false，置 throwOnNextSend 时抛出
             */
            Core::Task<bool> operator()(const std::string_view bytes)
            {
                if (isGated)
                {
                    co_await parkHere();
                }
                if (throwOnNextSend)
                {
                    throwOnNextSend = false;
                    throw std::runtime_error("发送回调抛出：FrameSender 的契约允许这么退出");
                }
                if (isRefusing)
                {
                    co_return false;
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

                void await_resume() const noexcept
                {
                }
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
            return [&path](const std::string_view bytes) { return path(bytes); };
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
        auto         lobbyFirstSub  = hub.subscribe("lobby", firstPeer);
        auto         lobbySecondSub = hub.subscribe("lobby", secondPeer);
        auto         otherSub       = hub.subscribe("news", otherPeer);

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

    /**
     * @brief 钉住：二进制扇出走的是二进制帧，且字节原样上线
     * @details 文本帧的负载按 RFC 6455 §5.6 必须是合法 UTF-8，对端的接收校验会因非法序列直接关连接
     *          （1007）。protobuf、图片这类字节唯一的活路是二进制帧，所以这条通道必须真的存在并且
     *          不改写负载——把 0xFF 塞进 publish() 是会被对端打回来的
     */
    TEST(WebSocketHub, BinaryPublishGoesOutAsABinaryFrameWithUntouchedBytes)
    {
        GatedSendPath path;
        WebSocketPeer peer{makeFrameSender(path)};
        WebSocketHub  hub;
        auto          subscription = hub.subscribe("lobby", peer);

        const std::string rawPayload("\xFF\xFE"
                                     "tail");
        drivePublish(hub.publishBinary("lobby", rawPayload));

        ASSERT_EQ(path.sentFrames.size(), 1U);
        EXPECT_EQ(frameOpCode(path.sentFrames[0]), static_cast<std::uint8_t>(WebSocketOpCode::Binary)) << "扇出把二进制当文本发：对端按 UTF-8 校验就会断开";
        EXPECT_TRUE(frameCarriesText(path.sentFrames[0], "tail"));
        EXPECT_NE(path.sentFrames[0].find('\xFF'), std::string::npos) << "负载被改写过了，不是原样上线的那串字节";
    }

    /**
     * @brief 钉住：帧类型跟着队列里的那一条走，而不是跟着正在写的发布者走
     * @details 一条连接同一时刻只有一个写者，所以文本发布者会替排在后面的二进制发布收尾。若类型记在
     *          发布者身上（或记在成员上），这条被合并的二进制就会以文本帧上线——上一用例看不出来，
     *          因为它没有并发发布者
     */
    TEST(WebSocketHub, QueuedBinaryKeepsItsFrameTypeWhenATextPublisherDrainsIt)
    {
        GatedSendPath path;
        path.isGated = true; // 第一帧停在闸门上：此后入队的都由这个文本发布协程带走
        WebSocketPeer peer{makeFrameSender(path)};
        WebSocketHub  hub;
        auto          subscription = hub.subscribe("lobby", peer);

        Core::Task<void> textPublish = hub.publish("lobby", "alpha");
        textPublish.handle().resume();
        ASSERT_TRUE(static_cast<bool>(path.parkedWriter)) << "闸门没起作用：第一帧根本没挂起";

        drivePublish(hub.publishBinary("lobby", std::string("\xEE\x80"
                                                            "blob")));

        path.release();
        textPublish.handle().promise().result();

        ASSERT_EQ(path.sentFrames.size(), 2U) << "合并后应当按序发出两条";
        EXPECT_EQ(frameOpCode(path.sentFrames[0]), static_cast<std::uint8_t>(WebSocketOpCode::Text));
        EXPECT_EQ(frameOpCode(path.sentFrames[1]), static_cast<std::uint8_t>(WebSocketOpCode::Binary)) << "替它收尾的写者是文本发布者，就按文本发了——类型必须跟每条走";
        EXPECT_TRUE(frameCarriesText(path.sentFrames[1], "blob"));
    }

    TEST(WebSocketHub, SubscriptionHandleRemovesTheMemberOnDestruction)
    {
        GatedSendPath path;
        WebSocketPeer peer{makeFrameSender(path)};
        WebSocketHub  hub;

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
        WebSocketHub  hub;

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
        WebSocketHub  hub;
        auto          subscription = hub.subscribe("lobby", peer);

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
        GatedSendPath         path;
        path.isGated = true;
        WebSocketPeer peer{makeFrameSender(path)};
        WebSocketHub  hub(kPendingByteBound);
        auto          subscription = hub.subscribe("lobby", peer);

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

    /**
     * @brief 钉住：丢弃计数在构造时就挂进进程读数表、导出的与 `droppedMessageCount()` 是同一个数、析构即注销
     * @details 只留在实例里的读数等于只有拿得到那个对象的人才知道在丢消息，而「扇出正在被慢读者拖累」
     *          正是需要在外面板上看见的那一格；把手漏注销会留下一条谁也不持有的读数，下一轮抓取还在报旧对象的值
     */
    TEST(WebSocketHubMetrics, ExportsDropsFromConstructionAndReleasesOnDestruction)
    {
        using AsynGyanis::TestSupport::findRegistrySample;

        EXPECT_FALSE(findRegistrySample("asyn_websocket_hub_dropped_messages_total").has_value()) << "还没有集线器，导出里就先有了这条读数";

        {
            GatedSendPath path;
            path.isGated = true;
            WebSocketPeer peer{makeFrameSender(path)};
            WebSocketHub  hub(16U);

            const auto registered = findRegistrySample("asyn_websocket_hub_dropped_messages_total");
            ASSERT_TRUE(registered.has_value()) << "构造时没挂上读数，运维面就看不见丢弃";
            EXPECT_EQ(registered->value, 0U) << "新登记的读数应当是 0，而不是上一位使用者的残留";

            auto subscription = hub.subscribe("lobby", peer);

            Core::Task<void> oversized = hub.publish("lobby", "0123456789abcdefghij"); // 20 字节 > 16 的上界
            oversized.handle().resume();
            EXPECT_EQ(hub.droppedMessageCount(), 1U);

            const auto sample = findRegistrySample("asyn_websocket_hub_dropped_messages_total");
            ASSERT_TRUE(sample.has_value());
            EXPECT_EQ(sample->value, static_cast<std::uint64_t>(hub.droppedMessageCount())) << "对外读数与判据用的不是同一个数";
        }

        EXPECT_FALSE(findRegistrySample("asyn_websocket_hub_dropped_messages_total").has_value()) << "集线器析构之后读数还挂在表上，就会报一个不存在的对象";
    }

    TEST(WebSocketHub, ClosedPeerIsNotTouchedAndItsQueueIsEmptyd)
    {
        GatedSendPath path;
        WebSocketPeer peer{makeFrameSender(path)};
        peer.markClosed(); // 会话侧收口：此后 send* 一律不该再被调用

        WebSocketHub hub;
        auto         subscription = hub.subscribe("lobby", peer);

        drivePublish(hub.publish("lobby", "after-close"));
        EXPECT_TRUE(path.sentFrames.empty()) << "对端已收口还去写它：单写者假设之外的字节没人负责";

        // 队列必须被排空而不是留着：一条收口的连接不该继续占内存
        drivePublish(hub.publish("lobby", "again"));
        EXPECT_TRUE(path.sentFrames.empty());
        EXPECT_EQ(hub.droppedMessageCount(), 0U) << "收口不是队满，不该记进丢弃计数";
        EXPECT_EQ(hub.abandonedMessageCount(), 2U) << "两次扇出都整队作废却无人认领：这笔账必须落在作废那一边";
    }

    /**
     * @brief 钉住写失败这条作废路：交出去没写成功的一条，和队列里剩下的，一起记
     * @details 集线器的 publish 语义是「尽力达」，返回时不保证字节上线；若不记这一笔，一条正在断的连接
     *          会让整段广播静默消失，而业务侧看到的仍是「publish 成功返回」
     */
    TEST(WebSocketHub, WriteFailureAbandonsTheMessageInFlightAndTheRestOfTheQueue)
    {
        GatedSendPath path;
        path.isGated = true;
        WebSocketPeer peer{makeFrameSender(path)};
        WebSocketHub  hub;
        auto          subscription = hub.subscribe("lobby", peer);

        Core::Task<void> first = hub.publish("lobby", "alpha");
        first.handle().resume();
        ASSERT_TRUE(static_cast<bool>(path.parkedWriter)) << "闸门没起作用：第一帧根本没挂起";
        drivePublish(hub.publish("lobby", "bravo")); // 排在队列里，由第一个写者带走

        path.isRefusing = true; // 放开闸门后这一帧写失败：对端随即收口，队列里那条也没有了对端
        path.release();
        first.handle().promise().result();

        EXPECT_TRUE(path.sentFrames.empty()) << "写失败的一帧不该留下上线字节";
        EXPECT_EQ(hub.abandonedMessageCount(), 2U) << "作废数该是「在途失败的那条 + 队列里剩下的那条」，少算在途那条就等于谎报送达";
        EXPECT_EQ(hub.droppedMessageCount(), 0U) << "这不是队满，不该挤进丢弃那本账";
    }

    /**
     * @brief 钉住写回调抛出这一条路：闩随展开复位，队列里剩下的不被抛弃
     * @details FrameSender 是业务给的回调，契约允许它抛（对端的写失败就是这么交回的）。原先闩只在
     *          正常收尾时复位：抛出去之后 isDraining 永远留在 true，这个成员此后再没人替它写——
     *          后来的发布只排队，涨到字节上界后整队被记成「队满丢弃」，面板给出的成因是错的。
     *          抛出那一帧确实没了（它已出队且随展开销毁），因此要记进作废；而队列里那些还活着，
     *          下一位写者照样带走，一条都不该记成没送出去。
     */
    TEST(WebSocketHub, ThrowingWriteReleasesTheMemberAndCountsOnlyTheFrameItConsumed)
    {
        GatedSendPath path;
        path.isGated = true;
        WebSocketPeer peer{makeFrameSender(path)};
        WebSocketHub  hub;
        auto          subscription = hub.subscribe("lobby", peer);

        Core::Task<void> first = hub.publish("lobby", "alpha"); // 出队后开写，停在闸门上
        first.handle().resume();
        ASSERT_TRUE(static_cast<bool>(path.parkedWriter)) << "闸门没起作用：第一帧根本没挂起";
        drivePublish(hub.publish("lobby", "bravo"));   // 排进队列，等第一个写者带走
        drivePublish(hub.publish("lobby", "charlie")); // 同上

        path.throwOnNextSend = true; // 放开闸门后那一帧抛出：异常该原样交回正在发布的那一位
        path.release();
        EXPECT_THROW(first.handle().promise().result(), std::runtime_error) << "写路径的抛出被集线器吞掉了";
        EXPECT_EQ(hub.abandonedMessageCount(), 1U) << "随展开销毁的那一帧没记进作废账";

        // 关键判据：闩已复位——下一次发布必须真的去写，并且把积压的两条按序一起带走
        path.isGated = false;
        drivePublish(hub.publish("lobby", "delta"));
        ASSERT_EQ(path.sentFrames.size(), 3U) << "抛出之后这个成员再也没人替它写：新的一条只排进了队列";
        EXPECT_TRUE(frameCarriesText(path.sentFrames[0], "bravo")) << "积压的条目没按到达顺序带走";
        EXPECT_TRUE(frameCarriesText(path.sentFrames[1], "charlie"));
        EXPECT_TRUE(frameCarriesText(path.sentFrames[2], "delta")) << "补写时把新的一条挤掉了：闩复位得不干净";
        EXPECT_EQ(hub.abandonedMessageCount(), 1U) << "队列里被带走的那两条不该被记成作废";
        EXPECT_EQ(hub.droppedMessageCount(), 0U) << "这条链路上从没发生过队满";
    }

    /**
     * @brief 钉住两本账的分界与导出：队满丢的进丢弃、没人收的进作废，两个读数各归各
     * @details 两本账指向不同的处置动作（前者调上界或修慢读者，后者是断连的正常代价），合成一条数就分不出现场。
     *          作废那条同样挂进进程读数表并构造即在：只留在实例里等于只有拿着那个对象的人才知道广播在整队消失
     */
    TEST(WebSocketHubMetrics, AbandonedAndDroppedLedgersStaySeparateAndBothExport)
    {
        using AsynGyanis::TestSupport::findRegistrySample;

        EXPECT_FALSE(findRegistrySample("asyn_websocket_hub_abandoned_messages_total").has_value()) << "还没有集线器，导出里就先有了这条读数";

        {
            constexpr std::size_t kPendingByteBound = 16U;
            GatedSendPath         path;
            path.isGated = true;
            WebSocketPeer peer{makeFrameSender(path)};
            WebSocketHub  hub(kPendingByteBound);

            const auto registered = findRegistrySample("asyn_websocket_hub_abandoned_messages_total");
            ASSERT_TRUE(registered.has_value()) << "构造时没挂上读数，运维面就看不见整队作废";
            EXPECT_EQ(registered->value, 0U) << "新登记的读数应当是 0，而不是上一位使用者的残留";

            auto subscription = hub.subscribe("lobby", peer);

            Core::Task<void> first = hub.publish("lobby", "1234567890"); // 出队后开写，停在闸门上
            first.handle().resume();
            ASSERT_TRUE(static_cast<bool>(path.parkedWriter));
            drivePublish(hub.publish("lobby", "abcdefghij")); // 排进队列：占用 10
            drivePublish(hub.publish("lobby", "klmnopqrst")); // 10+10 越界：丢的是这一条
            EXPECT_EQ(hub.droppedMessageCount(), 1U);

            subscription.reset(); // 这一侧先没人收了：在途那条照常写完，队列里那条随之作废
            path.release();
            first.handle().promise().result();

            EXPECT_EQ(path.sentFrames.size(), 1U) << "在途那条是写成功的，两头都不该记";
            EXPECT_EQ(hub.droppedMessageCount(), 1U) << "作废的这条挤进了丢弃账，两本账就合起来了";
            EXPECT_EQ(hub.abandonedMessageCount(), 1U) << "队列里没人收的那条该记在作废";

            const auto sample = findRegistrySample("asyn_websocket_hub_abandoned_messages_total");
            ASSERT_TRUE(sample.has_value());
            EXPECT_EQ(sample->value, static_cast<std::uint64_t>(hub.abandonedMessageCount())) << "对外读数与判据用的不是同一个数";
        }

        EXPECT_FALSE(findRegistrySample("asyn_websocket_hub_abandoned_messages_total").has_value()) << "集线器析构之后读数还挂在表上，就会报一个不存在的对象";
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
