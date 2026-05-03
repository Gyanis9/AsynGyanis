/**
 * @file TestHelpers.h
 * @brief 测试辅助工具 — ConfigManager 单例互斥锁，防止并行测试状态干扰
 * @copyright Copyright (c) 2026
 */
#ifndef TESTS_TESTHELPERS_H
#define TESTS_TESTHELPERS_H

#include <mutex>

/// 全局测试互斥锁：所有使用 ConfigManager 单例的测试必须持有此锁
inline std::mutex &configTestMutex()
{
    static std::mutex m;
    return m;
}

#endif
