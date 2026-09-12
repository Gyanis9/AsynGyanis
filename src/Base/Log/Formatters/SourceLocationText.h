/**
 * @file SourceLocationText.h
 * @brief 「文件:行号」文本的共用生成工具（短文件名场景零堆分配）
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Log/SourceLocation.h"

#include <format>
#include <span>
#include <string_view>

namespace AsynGyanis::Base
{
    /// 「文件:行号」栈上缓冲容量：足够容纳 50 余字符的文件名加 6 位行号，
    /// 本项目所有源文件名去目录后都远小于该值，只有异常长的文件名才回退到堆分配
    inline constexpr std::size_t kSourceLocationTextBufferSize = 64;

    /**
     * @brief 尝试把「文件:行号」写进调用方提供的缓冲，避免为短文件名分配
     *
     * @details 用 `std::format_to_n` 只往给定缓冲写、不分配，并返回「装下整段所需长度」；装得下就以
     *          string_view 指向缓冲内的文本，装不下才返回空视图，由调用方回退到分配路径（两条路径
     *          共用 `"{}:{}"`，文本与长度完全一致，只差内存来源）。
     *          空视图当且仅当未命中（长度恒不小于 2）；`noexcept` 成立是因为格式串是编译期字面量。
     * @param location 日志事件的源码位置
     * @param buffer 调用方提供的栈缓冲
     * @return std::string_view 命中时指向 buffer 内的文本；装不下时返回空视图（调用方自行回退）
     */
    [[nodiscard]] inline std::string_view tryFormatSourceLocationText(const SourceLocation &location, const std::span<char> buffer) noexcept
    {
        // n 的类型是 OutputIt 的差值类型，显式转换避免 size_t 隐式收窄的告警
        const auto [out, size] = std::format_to_n(buffer.data(),
                                                  static_cast<std::ptrdiff_t>(buffer.size()),
                                                  "{}:{}",
                                                  location.shortFileName(),
                                                  location.line);
        const auto requiredLength = static_cast<std::size_t>(size);
        if (requiredLength > buffer.size())
        {
            // 截断：缓冲里的前缀不完整，交给调用方走分配路径，避免输出半截「文件:行」
            return {};
        }
        return {buffer.data(), requiredLength};
    }
} // namespace AsynGyanis::Base
