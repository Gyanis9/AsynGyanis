/**
 * @file JsonWriteOptions.h
 * @brief JSON 序列化选项：缩进、ASCII 转义与深度守卫
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>

namespace AsynGyanis::Base
{
    /**
     * @brief JSON 序列化选项
     *
     * @details 默认值刻意与既有 JsonWriter::write(value, bool) 的输出格式完全一致：
     *          indentWidth = 2、ensureAscii = false，
     *          因此调用旧重载（indented = true）得到的历史输出逐字节不变。
     *
     * @note 这里**没有**「对象键序」选项：值模型的对象类型是按键升序的 std::map，
     *       写出顺序由容器本身决定且被既有用例与输出格式依赖，给一个改不动的开关
     *       只会让人以为设置了它就能改顺序。
     */
    struct JsonWriteOptions
    {
        std::size_t indentWidth{2};     ///< 每层缩进空格数；0 表示紧凑单行输出（不换行、不加空格）
        bool        ensureAscii{false}; ///< true 时把所有非 ASCII 字符转为 \uXXXX（补充平面用代理对），false 时原样输出 UTF-8 字节
        std::size_t maximumDepth{256};  ///< 序列化嵌套深度上限，0 表示不限制；超限抛 FormatError（kind 为 DepthExceeded）
    };
} // namespace AsynGyanis::Base
