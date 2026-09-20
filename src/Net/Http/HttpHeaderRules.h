/**
 * @file HttpHeaderRules.h
 * @brief HTTP 解析共用：头部字段规则与 OWS / ASCII 折叠等文本工具
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 报文解析的各层（请求行、字段值、分块尺寸行、WebSocket 头部）都要用同一套小工具，
 *          各写一份曾在仓里散出多份等价实现；规则与工具统一放在这里共用。
 */

#pragma once

#include <cstddef>
#include <string_view>

namespace AsynGyanis::Net
{
    /**
     * @brief 裁掉首尾的可选空白（OWS，RFC 9110 §5.6.3：SP 与 HTAB）
     * @param text 原始文本视图
     * @return std::string_view 去掉首尾 SP/HTAB 后的子视图，不复制字节
     */
    [[nodiscard]] inline std::string_view trimOptionalWhitespace(const std::string_view text) noexcept
    {
        std::size_t beginIndex = 0;
        std::size_t endIndex   = text.size();
        while (beginIndex < endIndex && (text[beginIndex] == ' ' || text[beginIndex] == '\t'))
        {
            ++beginIndex;
        }
        while (endIndex > beginIndex && (text[endIndex - 1] == ' ' || text[endIndex - 1] == '\t'))
        {
            --endIndex;
        }
        return text.substr(beginIndex, endIndex - beginIndex);
    }

    /**
     * @brief 按 ASCII 表把字符折叠为小写，非 A-Z 原样返回
     *
     * @details 不用 std::%tolower：它受 locale 影响（土耳其语环境下 'I' 折叠成非 ASCII 字节），
     *          而 HTTP 的名称与 token 都是按 ASCII 定义的，比较与归一化必须与区域设置无关。
     *
     * @param character 待折叠字符
     * @return char 折叠结果
     */
    [[nodiscard]] constexpr char toLowerAscii(const char character) noexcept
    {
        return (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character;
    }

    /**
     * @brief 逐字节比较两段 ASCII 文本（忽略字母大小写）
     *
     * @details 只折叠 A-Z 与 a-z：std::%tolower 受 locale 影响，非 ASCII 字节在不同平台上
     *          结果可能不同，头部比较若用它会在边界输入上出现平台差异。
     *
     * @param left 左操作数
     * @param right 右操作数
     * @return true 两者逐字节相等（忽略 ASCII 字母大小写）
     */
    [[nodiscard]] inline bool equalsIgnoringCase(const std::string_view left, const std::string_view right) noexcept
    {
        // 先比长度：长度不同直接为否，省掉逐字节循环
        if (left.size() != right.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < left.size(); ++index)
        {
            if (toLowerAscii(left[index]) != toLowerAscii(right[index]))
            {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 把十六进制字符转成数值
     * @param character 待转换字符
     * @return int 0-15；不是十六进制字符时为 -1
     */
    [[nodiscard]] inline int hexadecimalDigitValue(const char character) noexcept
    {
        if (character >= '0' && character <= '9')
        {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f')
        {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F')
        {
            return character - 'A' + 10;
        }
        return -1;
    }

    /**
     * @brief 判断一个响应头名是不是「连接特定」字段
     *
     * @details h1 口径的 `HttpResponse` 会带上 `transfer-encoding`、`connection`、`upgrade` 这类字段，
     *          而 h2（RFC 9113 §8.2.2）与 h3（RFC 9114 §4.2）禁止它们出现，带上会被对端判成报文格式
     *          错误（实测 h3 客户端直接回 MALFORMED_HTTP_HEADER）。h2/h3 装配响应头时都要剥掉，规则共用。
     * @param name 头名（调用方应已归一化为小写，与 HttpResponse 的存放口径一致）
     * @return true 该字段必须剥离
     */
    [[nodiscard]] bool isConnectionSpecificHeaderName(std::string_view name) noexcept;

    /**
     * @brief 严格解析 Content-Length 的取值（RFC 9110 §8.6）
     *
     * @details 只接受纯十进制数字：带后缀的「12abc」、带符号、带空格的数字一律拒绝——
     *          长度有歧义时「按哪个数读正文」会因实现而异，正是请求走私的入口。
     *          h1/h2 两侧共用这一份判定，避免两条承载对同一份头给出不同结论。
     *
     * @param text 字段值（允许带首尾 OWS，函数内部裁剪）
     * @param length 输出参数：解析出的长度（仅成功时写入）
     * @return true 取值合法
     * @return false 取值非法（空串、含非数字字符、超出 19 位十进制）
     */
    [[nodiscard]] bool parseContentLengthValue(std::string_view text, std::size_t &length) noexcept;
} // namespace AsynGyanis::Net
