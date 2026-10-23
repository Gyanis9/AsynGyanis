// CancellationWatch 单元测试：立即析构不得留下调度器指针，停止请求只执行一次动作

#include "Core/Coroutine/CancellationWatch.h"

#include "Core/EventLoop/EventLoop.h"

#include <gtest/gtest.h>

#include <stop_token>

namespace AsynGyanis::Core
{
    TEST(CancellationWatch, DestroyingBeforeFirstDispatchDoesNotUseFreedFrame)
    {
        EventLoop        loop;
        std::stop_source source;
        int              actionCount = 0;
        {
            CancellationWatch watch(loop, source.get_token(),
                                    [&actionCount](std::stop_token) -> Task<>
                                    {
                                        ++actionCount;
                                        co_return;
                                    });
        }

        // 看门狗在构造时被投递；若析构先于那次投递把它销毁，调度器会在这里踩到已释放帧
        loop.scheduler().runAll();
        EXPECT_EQ(actionCount, 0);
    }

    TEST(CancellationWatch, RunsTheActionOnceWhenTheTokenStops)
    {
        EventLoop         loop;
        std::stop_source  source;
        int               actionCount = 0;
        CancellationWatch watch(loop, source.get_token(),
                                [&actionCount, &loop](std::stop_token) -> Task<>
                                {
                                    ++actionCount;
                                    loop.stop();
                                    co_return;
                                });

        static_cast<void>(source.request_stop());
        loop.run();
        EXPECT_EQ(actionCount, 1);
    }
} // namespace AsynGyanis::Core
