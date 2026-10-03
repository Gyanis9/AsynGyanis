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

#include "AsynGyanisExport.h"

#include <array>
#include <cstddef>
#include <optional>
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
    [[nodiscard]] ASYN_NET_API bool isConnectionSpecificHeaderName(std::string_view name) noexcept;

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
    [[nodiscard]] ASYN_NET_API bool parseContentLengthValue(std::string_view text, std::size_t &length) noexcept;

    /**
     * @brief 判断内容类型头部是否指向指定的媒体类型
     *
     * @details 只看类型本身：`; charset=utf-8` 这类参数不参与判定，媒体类型大小写不敏感
     *          （RFC 9110 §8.3）。空视图判否——把没标类型的正文当成某种已知格式来解是凭空造数据。
     *          请求侧的 formFields()/jsonBody()/multipartForm() 与 `MultipartFormData::parse()` 共用，
     *          两条路对同一份头部给出不同结论时，「服务端认的格式」和「解析器认的格式」会分家。
     *
     * @param contentTypeHeader 内容类型头部取值（不含头部名），可为空视图
     * @param expectedMediaType 期望的媒体类型（小写书写）
     * @return true 匹配
     */
    [[nodiscard]] inline bool contentTypeIs(const std::string_view contentTypeHeader, const std::string_view expectedMediaType) noexcept
    {
        const std::size_t parameterPosition = contentTypeHeader.find(';');
        return equalsIgnoringCase(trimOptionalWhitespace(contentTypeHeader.substr(0, parameterPosition)), expectedMediaType);
    }
    /**
     * @brief 解析对端交来的状态码文本：只接受恰好三位十进制（RFC 9110 §4.1 的 status-code = 3DIGITS）
     *
     * @details 三条客户端通路共用这一处判据：h1 的状态行、h2 与 h3 的 `:status` 伪头。各写一份正是
     *          本仓反复出现过的漂移源，而这里漏判的代价不是「数字差一点」，是**拿到一个报文里
     *          从没写过的状态码**：`atoi("abc")` 交回 0、`strtoul("20")` 交回 20、`atoi("2000")` 交回 2000，
     *          上层按「2xx 才算成功」分支时，一个把状态码写坏的响应会伪装成一次失败或一次成功。
     *          所以这里只判形状不判范围：100..999 之外的三位数照样交回，取值是否合法由上层按自己的
     *          语义判（比如隧道只认 2xx）。
     * @param text 状态码文本（h1 取状态行里那一段，h2/h3 取伪头取值原文）
     * @return std::optional<int> 解析出的状态码；形状不合（长度不是 3、含非数字）时为空
     */
    [[nodiscard]] inline std::optional<int> parseStatusCodeText(const std::string_view text) noexcept
    {
        if (text.size() != 3)
        {
            return std::nullopt;
        }
        int value = 0;
        for (const char character: text)
        {
            if (character < '0' || character > '9')
            {
                return std::nullopt;
            }
            value = value * 10 + (character - '0');
        }
        return value;
    }
    /**
     * @brief 校验块扩展（chunk-ext）的语法
     * @details 按 RFC 9112 §7.1.1：扩展是若干「;名字[=值]」段，名字必须是 token，值可以是 token
     *          或带引号字符串；本框架只校验语法，不解释扩展的语义。
     *          两个方向共用这一份（先前只有入站 `HttpParser` 有一份文件内的实现，出站
     *          `HttpResponseParser` 直接忽略扩展内容）：扩展不参与块边界计算，但一个坏对端用非法
     *          扩展把本端一路读下去，正是缓存投毒与请求分裂的入口，所以忽略内容不等于忽略语法。
     * @param text 从第一个 ';' 起的扩展原文
     * @return true 语法合法
     */
    [[nodiscard]] inline bool areChunkExtensionsWellFormed(const std::string_view text) noexcept
    {
        const auto skipOptionalWhitespace = [&text](std::size_t &position) noexcept
        {
            while (position < text.size() && (text[position] == ' ' || text[position] == '\t'))
            {
                ++position;
            }
        };

        std::size_t offset = 0;
        while (offset < text.size())
        {
            // 每段都以 ';' 开头：段与段之间只允许 BWS，多出来的字节一律判非法
            if (text[offset] != ';')
            {
                return false;
            }
            ++offset;
            skipOptionalWhitespace(offset);

            // 扩展名必须是至少一个 token 字符
            const std::size_t nameBegin = offset;
            while (offset < text.size() && isTokenCharacter(static_cast<unsigned char>(text[offset])))
            {
                ++offset;
            }
            if (offset == nameBegin)
            {
                return false;
            }
            skipOptionalWhitespace(offset);

            if (offset < text.size() && text[offset] == '=')
            {
                ++offset;
                skipOptionalWhitespace(offset);
                if (offset < text.size() && text[offset] == '"')
                {
                    // 带引号字符串：内部允许 qdtext（HTAB、可见 ASCII、obs-text）与反斜杠转义
                    ++offset;
                    bool isClosed = false;
                    while (offset < text.size())
                    {
                        const unsigned char character = static_cast<unsigned char>(text[offset]);
                        if (character == '"')
                        {
                            ++offset;
                            isClosed = true;
                            break;
                        }
                        if (character == '\\')
                        {
                            // 引号对：反斜杠之后必须还有一个可打印字节，且不能是裸控制字符
                            if (offset + 1 >= text.size())
                            {
                                return false;
                            }
                            const unsigned char escaped = static_cast<unsigned char>(text[offset + 1]);
                            if (escaped < 0x20 && escaped != '\t')
                            {
                                return false;
                            }
                            offset += 2;
                            continue;
                        }
                        if (character == '\t' || (character >= 0x20 && character <= 0x7E) || character >= 0x80)
                        {
                            ++offset;
                            continue;
                        }
                        return false;
                    }
                    if (!isClosed)
                    {
                        return false;
                    }
                } else
                {
                    const std::size_t valueBegin = offset;
                    while (offset < text.size() && isTokenCharacter(static_cast<unsigned char>(text[offset])))
                    {
                        ++offset;
                    }
                    if (offset == valueBegin)
                    {
                        return false;
                    }
                }
                skipOptionalWhitespace(offset);
            }
        }
        return true;
    }

    /**
     * @brief 把响应状态码折成「能上线的那一个」：越界一律按 500 发
     * @details 状态码是 100..999 的三位数字（RFC 9110 §15）。越界在每条通道上都会出问题，但表现完全不同：
     *          h3 的连接层直接拒收这个 `:status`，整条流发不出东西；h1 则把原值写进状态行，对端按行
     *          解析时看到的是一行无法解释的状态字段。所以判定落在**上线之前的那一处**而不是 setter：
     *          `HttpResponse::setStatus()` 有意保持宽松（业务写错码时原值还要能在日志与调试里看见），
     *          各通道序列化时各自折回 500 并记一条自己的日志。两条通道共用这一份判据，别再各写一条
     * @param statusCode 业务给的状态码原值
     * @return int 合法值原样返回；越界返回 500
     */
    [[nodiscard]] inline int normalizeWireStatusCode(const int statusCode) noexcept
    {
        return statusCode < 100 || statusCode > 999 ? 500 : statusCode;
    }
} // namespace AsynGyanis::Net
