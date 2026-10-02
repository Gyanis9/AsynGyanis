/**
 * @file LogEscaping.h
 * @brief 把外部交来的文本折成可安全写进日志的形状
 * @author Gyanis
 * @date 2026-10-02
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace AsynGyanis::Base
{
    /**
     * @brief 把「不是自己写的」文本折成一行可打印文本
     *
     * @details 为什么需要：日志的一条记录对应一行，而控制字符一旦原样落进去就会破坏这件事——
     *          换行让一条记录在采集端变成两条（伪造的上下文能顶着别的时间戳与级别），回车让行首
     *          被覆盖，ESC 直接把转义序列送进终端（运维在屏幕上看到的颜色、光标位置甚至窗口标题
     *          都可能被改写），NUL 让按 C 字符串取日志的采集器把后半截静默丢掉。最麻烦的是这几个
     *          都**看不见**：日志看着完全正常，缺陷却已经发生。
     * @details 谁需要它：协议解析层已经挡掉了头部与请求目标里的控制字节（见 `HttpHeaderRules.h`
     *          的字段值字符集与 `HttpParser` 的目标字符集），所以本框架内部需要转义的地方是
     *          **「对端交来的自由文本」**——机构的 problem document、其它实现的错误串。这类文本
     *          合法地可以携带 `\n`（JSON 里就写成 `\\n`），解析后就是真换行，挡不住也不该挡。
     * @details 折法：可打印 ASCII（0x20..0x7E，不含 0x7F）原样保留，其余字节写成 `\xNN`。高位字节
     *          （UTF-8 的非 ASCII 序列）因此也会被写成转义形式——这是刻意的取舍：日志的读者要的是
     *          「这条说了什么」，而一段无法判定编码的字节在终端、文件与采集管道里的表现是三个样，
     *          宁可让它确定地难看。截断在超长时补一个标记，避免被误读成「原文就到此为止」。
     * @param text 待折的原文，可以是外部输入的任意字节
     * @param maximumDisplayByteCount 最多保留多少个原文字节，超出部分以截断标记表示
     * @return std::string 只含可打印 ASCII 与反斜杠转义的单行文本
     */
    [[nodiscard]] inline std::string escapeForLog(const std::string_view text, const std::size_t maximumDisplayByteCount = 48)
    {
        const std::string_view excerpt = text.substr(0, text.size() < maximumDisplayByteCount ? text.size() : maximumDisplayByteCount);
        std::string            result;
        result.reserve(excerpt.size() + 8);
        for (const char character: excerpt)
        {
            const auto byte = static_cast<unsigned char>(character);
            if (byte >= 0x20 && byte < 0x7F)
            {
                result.push_back(character);
                continue;
            }
            // 不可打印字节写成 \xNN：否则「值里夹了个 NUL」这类缺陷在日志里看着与正常值一模一样
            static constexpr char kHexDigits[] = "0123456789ABCDEF";
            result.append("\\x");
            result.push_back(kHexDigits[(byte >> 4) & 0x0FU]);
            result.push_back(kHexDigits[byte & 0x0FU]);
        }
        if (excerpt.size() < text.size())
        {
            result.append("…（已截断）");
        }
        return result;
    }
} // namespace AsynGyanis::Base
