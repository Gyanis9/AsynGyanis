/**
 * @file SecureCompare.h
 * @brief 秘密值的等值比较：长度与内容都要与「猜对几位」无关
 * @author Gyanis
 * @date 2026-09-29
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include <cstddef>
#include <string_view>

namespace AsynGyanis::Base
{
    /**
     * @brief 以恒定时间比较两个秘密值（Bearer 令牌、HMAC 摘要这类）
     * @details 逐字节短路比较会把「前几位对不对」泄漏进耗时里：攻击者不需要读内存，只要能测时间，
     *          就能一位一位地把令牌试出来。本实现把两段的每个字节都异或进累加器，走完整个较长段，
     *          耗时只由长度决定；长度差本身不视为秘密（令牌长度是公开的属性），因此不额外补齐。
     * @note 比较的是原始字节，不做规范化：Base64/十六进制的大小写、填充差异都会被当成不相等，
     *       调用方若接受多种写法要在比较之前自己归一
     * @param left 第一段
     * @param right 第二段
     * @return true 两段逐字节相同
     */
    [[nodiscard]] ASYN_BASE_API bool constantTimeEquals(std::string_view left, std::string_view right) noexcept;
} // namespace AsynGyanis::Base
