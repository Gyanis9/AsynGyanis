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

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
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
     * @brief 在一段文本里找一段子串，忽略 ASCII 字母大小写
     * @param text 被找的文本
     * @param needle 要找的子串；空串按「找得到」处理（与 `std::string_view::find` 同解）
     * @return true 找得到
     * @note 只折 ASCII 大小写：头部字段名与 token 按 RFC 9110 §5.1/§5.6.1 就是 ASCII，跟着 locale 走
     *       会把 ≥0x80 的字节也折进去（本仓此前有六份大小写折叠实现、其中三份跟着 locale 走）
     */
    [[nodiscard]] inline bool containsIgnoringCase(const std::string_view text, const std::string_view needle) noexcept
    {
        if (needle.size() > text.size())
        {
            return false;
        }
        for (std::size_t start = 0; start + needle.size() <= text.size(); ++start)
        {
            bool isMatched = true;
            for (std::size_t index = 0; index < needle.size(); ++index)
            {
                if (toLowerAscii(text[start + index]) != toLowerAscii(needle[index]))
                {
                    isMatched = false;
                    break;
                }
            }
            if (isMatched)
            {
                return true;
            }
        }
        return false;
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
     * @brief 判断 `te` 字段的取值是否就是 HTTP/2 与 HTTP/3 唯一放行那个 `trailers`
     *
     * @details 两条规范（RFC 9113 §8.2.2、RFC 9114 §4.2）都只放行这一个值，但比较的是 token 而不是
     *          字节串：token 大小写不敏感（RFC 9110 §5.6.2），字段值两侧的 OWS 不算内容
     *          （RFC 9110 §5.5）。按字节比会把 `te: Trailers` 这种合规范的写法判成协议错误。
     *          h3 一直按这条判，h2 此前写的是 `value != "trailers"`——同一条请求在两条通道上
     *          一个被收、一个被拒，而判它的那句话看起来两边都「写着 trailers」。
     *
     * @param value `te` 字段的原始取值（不含字段名），可为空视图
     * @return true 去掉两侧空白、忽略大小写之后恰好是 trailers
     */
    [[nodiscard]] inline bool teValueIsTrailers(const std::string_view value) noexcept
    {
        return equalsIgnoringCase(trimOptionalWhitespace(value), "trailers");
    }

    /**
     * @brief 判断 Transfer-Encoding 的取值是否恰好是唯一的 chunked
     *
     * @details RFC 9112 §6.1 要求 chunked 必须位于编码链的末尾，而本框架只实现 chunked 这一种
     *          传输编码：取值里出现 gzip 之类其它编码、或 chunked 重复出现，都判非法，绝不悄悄按
     *          identity 处理。入站（`HttpParser`）与出站（`HttpResponseParser`）共用这一份判据——
     *          一条判死、另一条退回「读到连接关闭」，同一份字节就会在两个方向上得到两种正文
     *          （拆不掉的那层要么留下分块框架、要么留下压缩字节，两边都不报错）。
     *
     * @param listValue 各条 Transfer-Encoding 取值按到达顺序以 ", " 连接后的原文
     * @return true 只有一个取值，且忽略大小写、去掉两侧 OWS 之后恰好是 chunked
     */
    [[nodiscard]] inline bool isSingleChunkedEncoding(const std::string_view listValue) noexcept
    {
        return equalsIgnoringCase(trimOptionalWhitespace(listValue), "chunked");
    }

    /**
     * @brief 判断一个字节能否出现在请求目标（URI 的 path/query 那一段）里
     *
     * @details 三条入站通路（h1 的请求行、h2 的 `:path`、h3 的 `:path`）吃的是同一条规则，此前却
     *          各写一份且两两不同：裸 `#` 与 `\` 只有 h3 拒，非 ASCII 字节只有 h1 拒。同一份资源
     *          因为走哪条通道而被认成两个，是路由与缓存键分家的形状。
     *
     * @details 现在这一份的取值范围：空格、DEL 与所有控制字符都不属于 URI 的字符集（空格还是请求行
     *          的分隔符，留着等于让一行变两行）；`#` 之后是片段，RFC 9110 §7.1 明写片段不属于请求
     *          目标，收到裸 `#` 就是目标畸形（要表达一个字符 `#` 得写 `%23`）；`\` 不在 `pchar` 里
     *          （RFC 3986 §3.3），而它长得像 Windows 的路径分隔符——收下来等于替对端做路径归一化的
     *          决定。**0x80..0xFF 是收的**：URI 的字符集只到 ASCII（RFC 3986 §2.1），裸的非 ASCII 字节
     *          是没转换就发出来的 IRI，而 IRI 转 URI 的约定编码就是 UTF-8（RFC 3987 §3.1）——按此解释而
     *          不判畸形，未编码的中文路径在真实客户端一直在发；且这些字节与 `%C3%A9` 解码之后是同一份
     *          内部表示，收下来并不新开任何表面。
     *
     * @param character 待判断的那个字节
     * @return true 可以出现在请求目标里
     */
    [[nodiscard]] inline bool isRequestTargetCharacter(const char character) noexcept
    {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x21U || byte == 0x7FU)
        {
            return false;
        }
        if (byte <= 0x7EU)
        {
            return character != '#' && character != '\\';
        }
        return true;
    }

    /**
     * @brief 判断一段文本是不是合法的 URI scheme（RFC 3986 §3.1：字母打头，其后字母/数字与 `+ - .`）
     *
     * @details 这条语法此前有两份各写一遍的实现（h3 的伪头校验、出站 URL 解析），而 h2 的 `:scheme`
     *          根本没有它——`ht:tp` 这种带冒号的值在 HTTP/2 通路上会被原样收进请求，而伪头里的冒号
     *          是把 URI 拆成两段的字符，收下来等于让下游按另一套切法理解同一个目标。判据收在这里之后
     *          三条通路共用一份，出站 URL 解析也走它。
     *
     * @param scheme 待判的那一段（`:scheme` 的取值，或 URL 里冒号之前那段）
     * @return true 可以作为协议名
     */
    [[nodiscard]] inline bool isUriSchemeSyntax(const std::string_view scheme) noexcept
    {
        const auto isAlpha = [](const char character) noexcept { return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z'); };
        if (scheme.empty() || !isAlpha(scheme.front()))
        {
            return false;
        }
        return std::ranges::all_of(scheme, [&isAlpha](const char character) noexcept
                                   { return isAlpha(character) || (character >= '0' && character <= '9') || character == '+' || character == '-' || character == '.'; });
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
    /**
     * @brief 把 Host/authority 文本收成主机比对键
     *
     * @details 去端口（IPv6 字面量连方括号一起留，`[::1]:8443` → `[::1]`）、折成小写 ASCII、
     *          去掉结尾的根点（`example.com.` 与 `example.com` 是同一个站点）。空文本交回空串，
     *          调用方据此走默认站点。
     * @note 这份归一化只有这一处实现，因为有两件事都要拿它比对：路由器按 Host 选虚拟主机，
     *       而 h2 要判 `Host` 与 `:authority` 指的是不是同一个实体（RFC 9113 §8.3.1 要求先按
     *       RFC 3986 §6.2 归一化再比）。两处各自折一遍，就会出现「协议判据说一致、选站说不同」
     *       或反过来的分叉——那种分叉本身就是请求走私的形状。
     *
     * @param authority 头部原文，允许带端口
     * @return std::string 比对键（无主机信息时为空串）
     */
    [[nodiscard]] inline std::string normalizeHostComparisonKey(const std::string_view authority)
    {
        std::string_view hostPart = trimOptionalWhitespace(authority);
        if (hostPart.empty())
        {
            return {};
        }

        if (hostPart.front() == '[')
        {
            // IPv6 字面量：方括号之内才是主机，']' 之后那段 ":端口" 不参与比对。
            // 没闭合的括号按原文处理（那是畸形 Host，交给比对自然落空）
            if (const std::size_t closingBracket = hostPart.find(']'); closingBracket != std::string_view::npos)
            {
                hostPart = hostPart.substr(0, closingBracket + 1);
            }
        } else if (const std::size_t colon = hostPart.find(':'); colon != std::string_view::npos)
        {
            hostPart = hostPart.substr(0, colon);
        }

        // 只留一个点的情形（Host 就是 "."）不去：它归一化后仍是 "."，比对必然落空
        while (hostPart.size() > 1 && hostPart.back() == '.')
        {
            hostPart.remove_suffix(1);
        }

        std::string normalized;
        normalized.reserve(hostPart.size());
        for (const char character: hostPart)
        {
            normalized.push_back(toLowerAscii(character));
        }
        return normalized;
    }
} // namespace AsynGyanis::Net
