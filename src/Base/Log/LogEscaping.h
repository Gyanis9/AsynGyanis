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
     * @brief 把「不是自己写的」文本折成一行，并直接续写进调用方的缓冲
     *
     * @details 折法与 escapeForLog 逐字节一致：可打印 ASCII（0x20..0x7E，不含 0x7F）原样保留，
     *          其余字节写成 \xNN，超长时截断并补一个截断标记。区别只在写法——结果落在调用方
     *          已有的缓冲里，不再为每一段文本造一份 owning 字符串。上下文这类「每行都要转义
     *          几个短字段」的形状因此不再逐行取堆。
     *          入口的 reserve 只是容量提示：它按原文字节数加一个常数预留，不保证覆盖所有增长。
     *          控制字节与高位字节各占四个字符，原文全是这类字节时，续写仍会让缓冲按几何增长扩容。
     * @param out 目标缓冲；不清空，转义文本追加在其现有内容之后
     * @param text 待折的原文，可以是外部输入的任意字节
     * @param maximumDisplayByteCount 最多保留多少个原文字节，超出部分以截断标记表示
     * @warning text 不得指向 out 自身：续写途中的 reserve 与 push_back 都可能让 out 重新分配，
     *          指向旧存储的视图随即悬空。
     */
    inline void appendEscapedForLog(std::string &out, const std::string_view text, const std::size_t maximumDisplayByteCount = 48)
    {
        const std::string_view excerpt = text.substr(0, text.size() < maximumDisplayByteCount ? text.size() : maximumDisplayByteCount);
        out.reserve(out.size() + excerpt.size() + 8U);
        static constexpr char kHexDigits[] = "0123456789ABCDEF";
        for (const char character: excerpt)
        {
            const auto byte = static_cast<unsigned char>(character);
            if (byte >= 0x20 && byte < 0x7F)
            {
                out.push_back(character);
                continue;
            }
            // 不可打印字节写成 \xNN：否则「值里夹了个 NUL」这类缺陷在日志里看着与正常值一模一样
            out.append("\\x");
            out.push_back(kHexDigits[(byte >> 4U) & 0x0FU]);
            out.push_back(kHexDigits[byte & 0x0FU]);
        }
        if (excerpt.size() < text.size())
        {
            out.append("…（已截断）");
        }
    }

    /**
     * @brief 把「不是自己写的」文本折成一行可打印文本
     *
     * @details 为什么需要：日志的一条记录对应一行，而控制字符一旦原样落进去就会破坏这件事——
     *          换行让一条记录在采集端变成两条（伪造的上下文能顶着别的时间戳与级别），回车让行首
     *          被覆盖，ESC 直接把转义序列送进终端（运维在屏幕上看到的颜色、光标位置甚至窗口标题
     *          都可能被改写），NUL 让按 C 字符串取日志的采集器把后半截静默丢掉。最麻烦的是这几个
     *          都看不见：日志看着完全正常，缺陷却已经发生。
     * @details 谁需要它：协议解析层已经挡掉了头部与请求目标里的控制字节（见 `HttpHeaderRules.h`
     *          的字段值字符集与 `HttpParser` 的目标字符集），所以本框架内部需要转义的地方是
     *          「对端交来的自由文本」——机构的 problem document、其它实现的错误串。这类文本
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
        std::string result;
        appendEscapedForLog(result, text, maximumDisplayByteCount);
        return result;
    }
} // namespace AsynGyanis::Base
