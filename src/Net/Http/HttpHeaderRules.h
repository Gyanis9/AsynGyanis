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

#include <array>
#include <cstddef>
#include <string_view>

namespace AsynGyanis::Net
{
    /**
     * @brief RFC 9110 §5.1 的 tchar 集合：下标是字节值，true 表示该字节可出现在 token 里
     * @details 用表而非逐字符比较：请求行与每条头部都要按字节过一遍判定，分支链在这里既慢
     *          又容易在边界写错。也不走 std::%isalnum——它随 locale 变化，GBK 一类区域设置下
     *          高位字节会被算成字母数字，把非 tchar 放行成合法头名。
     */
    inline constexpr std::array<bool, 256> kHttpTokenCharacterSet = []
    {
        std::array<bool, 256> characterSet{};
        // 按 string_view 遍历而不是遍历字符数组：后者会把结尾的 '\0' 也算进集合
        for (const std::string_view tokenCharacters: {"0123456789", "abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "!#$%&'*+-.^_`|~"})
        {
            for (const char character: tokenCharacters)
            {
                characterSet[static_cast<unsigned char>(character)] = true;
            }
        }
        return characterSet;
    }();

    static_assert(!kHttpTokenCharacterSet[0], "NUL 不属于 tchar");
    static_assert(!kHttpTokenCharacterSet[static_cast<unsigned char>(' ')], "SP 不属于 tchar");
    static_assert(!kHttpTokenCharacterSet[static_cast<unsigned char>('\t')], "HTAB 不属于 tchar");
    static_assert(!kHttpTokenCharacterSet[0x7F], "DEL 不属于 tchar");
    static_assert(!kHttpTokenCharacterSet[0xFF], "obs-text 不属于 tchar");
    static_assert(kHttpTokenCharacterSet[static_cast<unsigned char>('~')], "~ 属于 tchar");

    /**
     * @brief RFC 9110 §5.6.2 的字段值集合：下标是字节值，true 表示该字节可以出现在头字段值里
     * @details 放行 HTAB、可见 ASCII 与 obs-text（0x80 以上）；CR/LF/NUL/DEL 与其余控制字符一律
     *          判否——头部块以 CRLF 定界，放行的话一个字段值就能自己结束头部块。
     */
    inline constexpr std::array<bool, 256> kHttpFieldValueCharacterSet = []
    {
        std::array<bool, 256> characterSet{};
        characterSet[static_cast<unsigned char>('\t')] = true;
        // 循环变量用 int：unsigned char 推到 0xFF 之后再自增会回绕成 0，终止条件永不成立
        for (int byte = 0x20; byte <= 0xFF; ++byte)
        {
            // 0x7F（DEL）夹在 0x20-0xFF 中间，它是控制字符，不能随可见 ASCII 与 obs-text 一起放行
            characterSet[static_cast<std::size_t>(byte)] = byte != 0x7F;
        }
        return characterSet;
    }();

    static_assert(kHttpFieldValueCharacterSet[static_cast<unsigned char>(' ')], "SP 属于字段值");
    static_assert(kHttpFieldValueCharacterSet[static_cast<unsigned char>('\t')], "HTAB 属于字段值");
    static_assert(kHttpFieldValueCharacterSet[0x7E], "DEL 之前最后一个可见字符属于字段值");
    static_assert(kHttpFieldValueCharacterSet[0x80], "obs-text 下界属于字段值");
    static_assert(kHttpFieldValueCharacterSet[0xFF], "obs-text 上界属于字段值");
    static_assert(!kHttpFieldValueCharacterSet[static_cast<unsigned char>('\r')], "CR 不属于字段值");
    static_assert(!kHttpFieldValueCharacterSet[static_cast<unsigned char>('\n')], "LF 不属于字段值");
    static_assert(!kHttpFieldValueCharacterSet[0], "NUL 不属于字段值");
    static_assert(!kHttpFieldValueCharacterSet[0x7F], "DEL 不属于字段值");

    /**
     * @brief 判断字节是否为 RFC 9110 §5.1 的 tchar
     * @param character 待判断字节
     * @return true 表示可用作方法名、头部字段名等 token 的一部分
     */
    [[nodiscard]] constexpr bool isTokenCharacter(const unsigned char character) noexcept
    {
        return kHttpTokenCharacterSet[character];
    }

    /**
     * @brief 判断整段文本是否全部由 tchar 组成
     * @details 空文本按「未被反例否决」返回 true；名字/方法本身是否允许为空由调用方各自判定。
     * @param text 待判断文本
     * @return true 表示每个字节都是 tchar
     */
    [[nodiscard]] inline bool containsOnlyTokenCharacters(const std::string_view text) noexcept
    {
        for (const char character: text)
        {
            if (!kHttpTokenCharacterSet[static_cast<unsigned char>(character)])
            {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 判断字节是否可以出现在头字段值里
     * @param character 待判断字节
     * @return true 表示是 HTAB、可见 ASCII 或 obs-text
     */
    [[nodiscard]] constexpr bool isFieldValueCharacter(const unsigned char character) noexcept
    {
        return kHttpFieldValueCharacterSet[character];
    }

    /**
     * @brief 判断整段文本是否全部由合法的字段值字节组成
     * @details 空文本按「未被反例否决」返回 true。
     * @param text 待判断文本
     * @return true 表示不含 CR/LF/NUL/DEL 或其它非法控制字符
     */
    [[nodiscard]] inline bool containsOnlyFieldValueCharacters(const std::string_view text) noexcept
    {
        for (const char character: text)
        {
            if (!kHttpFieldValueCharacterSet[static_cast<unsigned char>(character)])
            {
                return false;
            }
        }
        return true;
    }

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
     * @brief 判断字符串是否是合法的 HTTP 头部字段名
     * @details 按 RFC 9110 §5.1 的 tchar 集合校验，请求侧与响应侧共用同一张表：两侧判定不一致时，
     *          同一段转发代码会在「收得进来、发不出去」之间分裂。空白、冒号与控制字符都
     *          让报文无法定界，一律拒绝；空名字同样非法。
     * @param fieldName 头部字段名，原样判定（字符集与大小写无关）
     * @return true 可作为头部字段名
     */
    [[nodiscard]] inline bool isValidHeaderFieldName(const std::string_view fieldName) noexcept
    {
        return !fieldName.empty() && containsOnlyTokenCharacters(fieldName);
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
