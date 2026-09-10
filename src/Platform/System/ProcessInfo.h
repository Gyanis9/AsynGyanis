/**
 * @file ProcessInfo.h
 * @brief 进程级平台信息：可执行文件目录与环境变量读取
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/Platform.h"

#include <filesystem>
#include <optional>
#include <string>

namespace AsynGyanis::Platform
{
    /**
     * @brief 当前进程的平台信息入口
     *
     * @details 日志与配置的相对路径必须基于可执行文件所在目录解析，否则会随
     *          进程启动时的工作目录变化而漂移；环境变量读取在 MSVC 上必须使用
     *          _dupenv_s 才能避开 C4996 与内存归属问题，两者统一由本类承担。
     */
    class ProcessInfo
    {
    public:
        /**
         * @brief 获取可执行文件所在目录
         * @return std::filesystem::path 绝对目录路径；平台查询失败时返回空路径
         */
        static std::filesystem::path applicationDirectory();

        /**
         * @brief 读取当前进程的环境变量
         * @param variableName 环境变量名
         * @return std::optional<std::string> 变量存在时返回其值，未定义时返回 std::nullopt
         */
        static std::optional<std::string> environmentVariable(const std::string &variableName);
    };
} // namespace AsynGyanis::Platform
