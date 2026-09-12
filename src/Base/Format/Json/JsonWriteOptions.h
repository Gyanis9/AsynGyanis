/**
 * @file JsonWriteOptions.h
 * @brief JSON 序列化选项：缩进、ASCII 转义、键序与深度守卫
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace AsynGyanis::Base
{
    /**
     * @brief JSON 序列化选项
     *
     * @details 默认值刻意与既有 JsonWriter::write(value, bool) 的输出格式完全一致：
     *          indentWidth = 2、ensureAscii = false、keyOrder = Sorted，
     *          因此调用旧重载（indented = true）得到的历史输出逐字节不变。
     */
    struct JsonWriteOptions
    {
        /**
         * @brief 对象成员的输出顺序策略
         */
        enum class KeyOrder : std::uint8_t
        {
            Sorted, ///< 按键升序输出（默认，与历史行为一致）
            AsIs    ///< 按容器当前迭代顺序输出，不额外重排
        };

        std::size_t indentWidth{2};             ///< 每层缩进空格数；0 表示紧凑单行输出（不换行、不加空格）
        bool        ensureAscii{false};         ///< true 时把所有非 ASCII 字符转为 \uXXXX（补充平面用代理对），false 时原样输出 UTF-8 字节
        KeyOrder    keyOrder{KeyOrder::Sorted}; ///< 对象键序策略
        std::size_t maximumDepth{256};          ///< 序列化嵌套深度上限，0 表示不限制；超限抛 FormatError（kind 为 DepthExceeded）
    };
} // namespace AsynGyanis::Base
