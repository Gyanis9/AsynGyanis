/**
 * @file ConfigValue.h
 * @brief 配置值类型别名，指向 Format 的统一文档值
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Config/ConfigValueType.h"
#include "Base/Format/Value/FormatValue.h"

namespace AsynGyanis::Base
{
    /**
     * @brief 配置值类型别名集合
     *
     * @details 值模型已归入 Base/Format/Value/FormatValue.h 并由 JSON/YAML 等解析器共用，
     *          配置侧保留原有名字，避免既有代码与用例无意义改名；
     *          两侧是同一个类型，不存在第二份实现或相互转换开销。
     */
    using ConfigValue  = FormatValue;       ///< 配置值即文档值
    using ConfigArray  = FormatValueArray;  ///< 配置数组
    using ConfigObject = FormatValueObject; ///< 配置对象
}
