/**
 * @file TestHelpers.h
 * @brief 测试辅助工具：ConfigManager 单例的互斥锁，防止并行用例互相踩状态
 * @author Gyanis
 * @date 2026-05-03
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once

#include <mutex>

/// 全局测试互斥锁：所有使用 ConfigManager 单例的测试必须持有此锁
inline std::mutex &configTestMutex()
{
    static std::mutex m;
    return m;
}
