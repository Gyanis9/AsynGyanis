/**
 * @file ConfigLoadResult.h
 * @brief 配置加载结果结构与热加载回调类型
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace AsynGyanis::Base
{
    /**
     * @brief 配置加载结果
     *
     * @details 一次加载/重载的完整回执：成功标记、成功与失败的文件清单、
     *          错误明细以及完成时间戳，供调用方逐条上报或断言。
     * @note 部分文件加载失败时，成功文件里的键照常生效，失败文件里的键会从快照中消失
     *       （后续读回默认值）。需要「全成功才切换」语义的调用方应先检查 success 与
     *       failedFiles，再决定如何使用本次结果。
     */
    struct ConfigLoadResult
    {
        bool                     success{false}; ///< 加载是否成功
        std::vector<std::string> loadedFiles;    ///< 成功加载的配置文件路径列表
        std::vector<std::string> failedFiles;    ///< 加载失败的配置文件路径列表
        std::vector<std::string> errors;         ///< 加载过程中产生的错误信息列表

        std::chrono::steady_clock::time_point timestamp; ///< 加载完成时的时间戳

        /**
         * @brief 隐式转换为 bool，表示加载是否成功。
         * @return 成功返回 true。
         */
        explicit operator bool() const noexcept
        {
            return success;
        }
    };

    /**
     * @brief 热加载事件回调类型
     * @param result 本次重载结果。
     */
    using HotReloadCallback = std::function<void(const ConfigLoadResult &result)>;
} // namespace AsynGyanis::Base
