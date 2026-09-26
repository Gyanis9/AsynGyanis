/**
 * @file PlainTextLogLine.h
 * @brief 一行纯文本日志的组装：DefaultFormatter 与 ColorFormatter 共用的版式定义
 * @author Gyanis
 * @date 2026-09-23
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Log/Formatters/PaddedFieldText.h"
#include "Base/Log/Formatters/SourceLocationText.h"
#include "Base/Log/Formatters/StackTraceText.h"
#include "Base/Log/Formatters/TimestampText.h"
#include "Base/Log/LogEvent.h"
#include "Base/Log/LogLevel.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace AsynGyanis::Base::Detail
{
    /// 除变长字段之外那截框架的开销：时刻文本 23 格、等级 5 格、方括号与分隔空格若干
    inline constexpr std::size_t kPlainTextLineFrameOverheadBytes = 40U;

    /**
     * @brief 把一条日志事件追加成一行纯文本（不含行尾换行），等级两侧可插入修饰码
     * @details 版式只在这里定义一次：带色与不带色两条实现原先各写一遍同样的字段顺序与分隔符，
     *          改格式必须同时改两处。颜色码只夹在等级两侧、不参与补齐，因此留出前后缀即可，
     *          无色版传两个空视图。
     * @param out 目标缓冲；不清空，本次文本追加在其现有内容之后
     * @param event 日志事件
     * @param levelPrefix 等级字段之前的修饰（如 ANSI 颜色码），可为空
     * @param levelSuffix 等级字段**补齐之后**的收尾（如颜色复位），可为空
     */
    inline void appendPlainTextLogLine(std::string &out, const LogEvent &event, const std::string_view levelPrefix, const std::string_view levelSuffix)
    {
        // 时刻在本线程就地渲染成文本：一块栈缓冲，不取堆。两条版式分支只走其中一条，
        // 因此一次渲染一份缓冲就够
        std::array<char, kTimestampTextBufferSize> timestampBuffer{};
        const std::string_view                     timestampText = formatTimestampText(timestampBuffer, event.timestamp);
        const std::string_view                     levelText{logLevelToString(event.level)};
#ifdef ASYN_DEBUG
        // 源码位置经共用工具生成：短「文件:行号」写进栈缓冲，装不下才回退到会分配的路径
        std::array<char, kSourceLocationTextBufferSize> locationBuffer{};
        std::string                                     locationOverflow;

        const std::string_view location = formatSourceLocationText(event.location, locationBuffer, locationOverflow);

        // 逐字段追加而不是 std::format_to + back_insert_iterator：后者要一个字符一个字符地喂给
        // 输出迭代器并当场解析格式串，整行下来比按字段 memcpy 慢数倍。
        // 字段顺序与分隔符就是格式串 "{} {} [{}{:<5}{}] [{}] {:<13} {}"（无色时前后缀为空）
        out.reserve(out.size() + kPlainTextLineFrameOverheadBytes + levelPrefix.size() + levelSuffix.size() + event.threadIdView().size() + event.loggerNameView().size() +
                    location.size() + event.message.size());
        out.append(timestampText);
        out.push_back(' ');
        out.append(event.threadIdView());
        out.append(" [");
        out.append(levelPrefix);
        appendPaddedField(out, levelText, kLevelFieldWidth);
        out.append(levelSuffix);
        out.append("] [");
        out.append(event.loggerNameView());
        out.append("] ");
        appendPaddedField(out, location, kSourceLocationFieldWidth);
        out.push_back(' ');
        out.append(event.message);
#else
        // Release：不输出线程号与源码位置，只保留定位问题必需的字段
        out.reserve(out.size() + kPlainTextLineFrameOverheadBytes + levelPrefix.size() + levelSuffix.size() + event.loggerNameView().size() + event.message.size());
        out.append(timestampText);
        out.append(" [");
        out.append(levelPrefix);
        appendPaddedField(out, levelText, kLevelFieldWidth);
        out.append(levelSuffix);
        out.append("] [");
        out.append(event.loggerNameView());
        out.append("] ");
        out.append(event.message);
#endif
        // 栈的符号解析在调用方（Sink）选定的线程上发生，因此随本函数一起落地
        appendStackTraceText(out, event.stackTrace);
    }
} // namespace AsynGyanis::Base::Detail
