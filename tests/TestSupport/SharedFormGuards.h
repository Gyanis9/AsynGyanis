/**
 * @file SharedFormGuards.h
 * @brief 交付形态换掉观测手段时，让依赖该观测手段的用例按 SKIP 报出的守卫（而不是让它假绿或假红）
 * @author Gyanis
 * @date 2026-09-29
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 库以共享形态提供时，可执行体与各模块 DLL/SO 各持一份静态链接的 OpenSSL。用例里
 *          「服务端上下文来自库、客户端 SSL_new 来自测试侧」这种接法跨的是两个 OpenSSL 实例：
 *          错误队列按实例各一份，握手成败仍然由记录层决定，但「拒绝原因」这类读的是队列的断言
 *          测的已经不是被测语义。分配探针的同类判据在 AllocationProbe.h 里。
 */

#pragma once

namespace AsynGyanis::TestSupport
{
    /**
     * @brief 用例侧与库侧是否各持一份静态链接的 OpenSSL（库以共享形态提供时为真）
     * @note 判据写在 SharedFormGuards.cpp：头里的 constexpr 值会让 MSVC 把 GTEST_SKIP() 之后的
     *       用例体判成不可达代码（C4702），本仓库 /WX 下即错误。
     */
    extern const bool kTlsHarnessIsCrossInstance;
} // namespace AsynGyanis::TestSupport

/// TLS 直连用例的开场守卫：跨实例的接法下这些断言不再指向被测语义，用例按 SKIP 报出。
/// 写成宏是因为 GTEST_SKIP() 只能在用例函数体内展开——放进助手函数里会让用例继续跑完。
#define ASYN_SKIP_IF_TLS_HARNESS_IS_CROSS_INSTANCE()                                                                                                                               \
    do                                                                                                                                                                             \
    {                                                                                                                                                                              \
        if (::AsynGyanis::TestSupport::kTlsHarnessIsCrossInstance)                                                                                                                 \
        {                                                                                                                                                                          \
            GTEST_SKIP() << "库以共享形态提供：用例侧与库侧各持一份静态链接的 OpenSSL，跨实例的 SSL_CTX/SSL 与错误队列不是被测语义";                                               \
        }                                                                                                                                                                          \
    } while (false)
