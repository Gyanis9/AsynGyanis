// Cancelable 单元测试：停止请求、停止令牌、停止源与移动语义

#include "Core/Coroutine/Cancelable.h"

#include <gtest/gtest.h>

namespace AsynGyanis::Core
{
    /**
     * @brief 新构造的 Cancelable 处于「未请求停止」态：对象存在本身不构成停止信号
     */
    TEST(Cancelable, DefaultStateIsNotStopped)
    {
        const Cancelable cancelable;

        EXPECT_FALSE(cancelable.isStopRequested());
    }

    /**
     * @brief requestStop() 的返回值是「本次调用是否完成状态翻转」：首次请求返回 true，且状态查询同步变为已停止
     */
    TEST(Cancelable, RequestStopSetsStopRequestedFlag)
    {
        Cancelable cancelable;

        EXPECT_TRUE(cancelable.requestStop());
        EXPECT_TRUE(cancelable.isStopRequested());
    }

    /**
     * @brief stopToken() 交出的令牌与停止源共享状态：请求停止后，已发出的令牌同步可见（是共享而非快照）
     */
    TEST(Cancelable, StopTokenReflectsStopState)
    {
        Cancelable cancelable;
        const auto token = cancelable.stopToken();

        EXPECT_FALSE(token.stop_requested());

        // 令牌与停止源共享状态，请求停止后令牌同步可见
        cancelable.requestStop();
        EXPECT_TRUE(token.stop_requested());
    }

    /**
     * @brief 移动构造让停止状态跟随对象迁移：源上已置位的停止请求在新对象上仍然可见
     */
    TEST(Cancelable, MoveConstructionPreservesStopState)
    {
        Cancelable source;
        source.requestStop();

        const Cancelable target(std::move(source));

        EXPECT_TRUE(target.isStopRequested());
    }

    /**
     * @brief 移动赋值同样带走停止状态，覆盖目标对象原有的「未停止」状态
     */
    TEST(Cancelable, MoveAssignmentPreservesStopState)
    {
        Cancelable source;
        source.requestStop();

        Cancelable target;
        target = std::move(source);

        EXPECT_TRUE(target.isStopRequested());
    }

    /**
     * @brief stopSource() 返回内部停止源的引用而非副本：调用方直接 request_stop() 也能反映到 isStopRequested()
     */
    TEST(Cancelable, StopSourceReturnsModifiableReference)
    {
        Cancelable cancelable;
        auto &source = cancelable.stopSource();

        EXPECT_FALSE(source.stop_requested());

        // 直接操作停止源应同步反映到 Cancelable 的状态查询
        source.request_stop();
        EXPECT_TRUE(cancelable.isStopRequested());
    }

    /**
     * @brief requestStop() 幂等：重复请求不会重复触发，第二次返回 false 且状态保持已停止
     */
    TEST(Cancelable, RepeatedRequestStopIsIdempotent)
    {
        Cancelable cancelable;

        // 第一次调用返回 true（从未停止转为已停止）
        EXPECT_TRUE(cancelable.requestStop());
        // 第二次调用返回 false（已经处于停止状态）
        EXPECT_FALSE(cancelable.requestStop());
        // 状态查询保持为已停止
        EXPECT_TRUE(cancelable.isStopRequested());
    }
} // namespace AsynGyanis::Core
