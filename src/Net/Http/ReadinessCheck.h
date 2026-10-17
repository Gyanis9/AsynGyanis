/**
 * @file ReadinessCheck.h
 * @brief 就绪探针的依赖检查回调形状
 * @author Gyanis
 * @date 2026-10-17
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include <functional>
#include <string>

namespace AsynGyanis::Net
{
    /**
     * @brief 一次依赖检查的结论
     * @details `reason` 会原样进 `/readyz` 的正文，不得包含连接串、口令或其它凭据；
     *          它只该说清「哪一格不就绪」（如「数据库连接池不可用」）。
     */
    struct ASYN_NET_API ReadinessCheckResult
    {
        bool        ready{true}; ///< 依赖是否可用
        std::string reason{};    ///< 不就绪时点名哪一格（就绪时为空）
    };

    /**
     * @brief 就绪探针的依赖检查回调
     *
     * @details 每次抓取 `/readyz` 时在服务器的事件循环线程上调用一次。回调必须是 O(1) 的
     *          快照读取（读原子量或加锁保护的上一轮探测结果），不得在这里做网络或磁盘 IO：
     *          探针会被编排器高频抓取，阻塞回调等于阻塞整条循环。真实依赖的探测应由调用方
     *          自己的后台协程/线程完成，这里只交「上一轮探测的结论」。
     */
    using ReadinessCheck = std::function<ReadinessCheckResult()>;
} // namespace AsynGyanis::Net
