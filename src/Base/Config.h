/**
 * @file Config.h
 * @brief 配置管理器（ConfigManager 别名），提供 YAML/JSON/INI 配置加载
 * @copyright Copyright (c) 2026
 */
#ifndef BASE_CONFIG_H
#define BASE_CONFIG_H

#include "ConfigManager.h"

namespace Base
{
    /// @brief Config 是 ConfigManager 的类型别名，提供配置加载、热更新、类型安全访问。
    using Config = ConfigManager;
}

#endif
