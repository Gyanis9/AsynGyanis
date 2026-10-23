// CancellationWatch 单元测试：立即析构不得留下调度器指针，停止请求只执行一次动作

// GCC 13 在 -O3 下对 std::optional<Core::Task<>> 里的协程句柄报 maybe-uninitialized：
// 告警落在 /usr/include/c++/13/coroutine:232，Debug、MSVC 与 Clang 都不报。只在本用例
// 文件内关掉这一条（告警位置在系统头里，必须放在 include 之前才能盖住）。
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

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
