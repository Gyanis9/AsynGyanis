/**
 * @file DefaultFormatter.cpp
 * @brief 默认纯文本日志格式化器
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Log/Formatters/DefaultFormatter.h"
#include "Base/Log/LogLevel.h"

#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>

namespace AsynGyanis::Base
{
    namespace
    {
        /// 「文件:行号」栈上缓冲容量：足够容纳 50 余字符的文件名加 6 位行号，
        /// 本项目所有源文件名去目录后都远小于该值，只有异常长的文件名才回退到堆分配
        constexpr std::size_t kSourceLocationBufferSize = 64;
    } // namespace

    std::string DefaultFormatter::format(const LogEvent &event)
    {
#ifdef ASYN_DEBUG
        // 「文件:行号」先写进栈上缓冲：嵌套 std::format 既要多跑一次格式化，又让每行
        // 多出一次堆分配（实测每行分配 2 次，其中一次就来自这段临时串）。
        // format_to_n 只往给定缓冲写、不分配，并返回「装下整段所需长度」：
        // 装得下就直接以 string_view 参与外层格式化，装不下才回退到原来的分配路径。
        // 两条路径共用同一套格式说明，输出逐字节一致（含 {:<13} 用空格补齐到 13 列）
        std::array<char, kSourceLocationBufferSize> locationBuffer{};
        const auto writtenLocation = std::format_to_n(locationBuffer.data(), kSourceLocationBufferSize, "{}:{}", event.location.shortFileName(),
                                                      event.location.line);
        const std::size_t locationSize = static_cast<std::size_t>(writtenLocation.size);
        const std::string overflowLocation = locationSize > kSourceLocationBufferSize
                                                 ? std::format("{}:{}", event.location.shortFileName(), event.location.line)
                                                 : std::string();
        const std::string_view location = locationSize <= kSourceLocationBufferSize
                                              ? std::string_view(locationBuffer.data(), locationSize)
                                              : std::string_view(overflowLocation);

        return std::format("{} {} [{:<5}] [{}] {:<13} {}",
                           event.timestamp,
                           event.threadId,
                           logLevelToString(event.level),
                           event.loggerNameView(),
                           location,
                           event.message);
#else
        // Release：不输出线程号与源码位置，只保留定位问题必需的字段
        return std::format("{} [{:<5}] [{}] {}",
                           event.timestamp,
                           logLevelToString(event.level),
                           event.loggerNameView(),
                           event.message);
#endif
    }
} // namespace AsynGyanis::Base
