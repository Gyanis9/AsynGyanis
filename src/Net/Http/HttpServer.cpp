#include "Net/Http/HttpServer.h"

#include "Net/Http/HttpMemoryBudget.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Task.h"
#include "Core/Metrics/ProcessMetricsRegistry.h"
#include "Net/Http/FileSender.h"
#include "Net/Http/HttpConditionalValidators.h"
#include "Net/Http/HttpDate.h"
#include "Net/Http/HttpHeaderRules.h"
#include "Net/Http/HttpMetricsEndpoint.h"
#include "Net/Http/HttpSession.h"
#include "Net/Http2/Http2Session.h"
#include "Platform/FileSystem/FileBasicInfo.h"
#include "Platform/FileSystem/FileSystem.h"
#include "Platform/IO/FileContents.h"
#include "Platform/IO/MemoryMappedFile.h"
#include "Platform/Platform.h"

#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 单个静态文件允许作为正文下发的上限，单位字节（64 MiB）
         * @details 正文走内存映射（不经过堆缓冲），但整份文件仍要一次性上线：
         *          对端读得慢时，这次发送会把整份文件挂在连接上，服务端没有分块续传的收尾策略，
         *          因此必须设上限。正文在 Linux 上已由内核零拷贝搬运，但慢消费者期间这份映射
         *          仍旧一直挂着；需要服务更大文件时得先做出分块续传的收尾策略。
         */
        constexpr std::uintmax_t kMaximumStaticFileSize = 64ull * 1024 * 1024;

        /// 一个百分号转义序列的固定长度：'%' 加两位十六进制
        constexpr std::size_t kPercentEscapeSequenceLength = 3;

        /**
         * @brief 路径清洗的结论
         */
        enum class PathVerdict
        {
            Accepted,  ///< 通过全部检查，relativeText 可直接拼进根目录
            Malformed, ///< 请求路径本身畸形（无 '/' 开头、含空字节、转义非法）→ 400
            Forbidden  ///< 形态可疑但未畸形（穿越段、反斜杠、盘符、绝对路径）→ 403
        };

        /**
         * @brief 判断十六进制字符
         * @param character 待判定字符
         * @return true 是 [0-9a-fA-F]
         */
        constexpr bool isHexadecimalDigit(const char character)
        {
            return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F');
        }

        /**
         * @brief 路径段的百分号解码
         *
         * @details 与 HttpRequest::percentDecode()（私有、服务查询串）刻意不共用一份实现：
         *          那边把 '+' 当成空格（application/x-www-form-urlencoded 约定），路径段里没有
         *          这条约定；此处还要求转义序列必须完整合法——残缺的 "%2" 想表达什么只有客户端
         *          知道，宁可判畸形也不替它猜。
         *
         * @param encoded 待解码的路径原文
         * @param decoded 输出解码结果（失败时内容不保证，调用方应丢弃）
         * @return true 解码成功
         * @return false 出现残缺或非法的百分号转义序列
         */
        bool decodePathEscapes(const std::string_view encoded, std::string &decoded)
        {
            decoded.clear();
            decoded.reserve(encoded.size());

            for (std::size_t index = 0; index < encoded.size(); ++index)
            {
                const char currentCharacter = encoded[index];
                if (currentCharacter != '%')
                {
                    decoded.push_back(currentCharacter);
                    continue;
                }

                // 界内判据用「剩余长度够不够一个完整序列」，正好覆盖以 "%41" 收尾的情形
                if (encoded.size() - index < kPercentEscapeSequenceLength)
                {
                    return false;
                }
                if (!isHexadecimalDigit(encoded[index + 1]) || !isHexadecimalDigit(encoded[index + 2]))
                {
                    return false;
                }

                const int decodedByteValue = hexadecimalDigitValue(encoded[index + 1]) * 16 + hexadecimalDigitValue(encoded[index + 2]);

                // 空字节：C 接口与部分文件系统会把它当字符串结尾，一旦混进路径就能截断文件名。
                // 这是解码之后才能发现的攻击，光看原文永远看不到 '\0'
                if (decodedByteValue == 0)
                {
                    return false;
                }

                decoded.push_back(static_cast<char>(decodedByteValue));
                index += 2; // 跳过刚消费掉的两位十六进制，循环自增再吃掉 '%'
            }
            return true;
        }

        /**
         * @brief 判断路径是否写成「绝对形态」
         * @details '//' 开头是协议相对 URL（后面往往跟着主机名），单反斜杠开头是 Windows UNC 的残段，
         *          两者都不可能是根目录之下的相对路径。
         * @param decodedPath 已解码的路径
         * @return true 属于绝对形态，应当拒绝
         */
        bool isAbsoluteFormPath(const std::string_view decodedPath)
        {
            return decodedPath.starts_with("//") || decodedPath.starts_with("/\\") || decodedPath.starts_with("\\");
        }

        /**
         * @brief 把请求路径清洗成「可以安全拼到静态根目录之下的相对路径」
         *
         * @details 每一步防的都是一个具体攻击，顺序不能调换：先要求以 '/' 开头（绝对形式 URI 与
         *          OPTIONS 的 "*" 在此出局），裸 CR/LF 直接判畸形（换行是头部分隔符）；反斜杠与
         *          冒号整体拒绝（Windows 下可注入盘符与 NTFS 交替数据流 "file.txt::$DATA"）；
         *          百分号解码必须先于分段判断（否则 "%2e%2e%2f" 能穿过只看字面的比较）。
         *
         * @param rawPath 请求路径原文（未解码，不含查询串）
         * @param relativeText 输出：相对根目录的路径文本，使用 '/' 分隔且不含前导 '/'
         * @return PathVerdict 清洗结论
         */
        PathVerdict sanitizeRequestPath(const std::string_view rawPath, std::string &relativeText)
        {
            relativeText.clear();

            if (rawPath.empty() || rawPath.front() != '/')
            {
                return PathVerdict::Malformed;
            }

            // 裸换行：路径不可能含 CR/LF，出现即为畸形报文
            if (rawPath.find('\r') != std::string_view::npos || rawPath.find('\n') != std::string_view::npos)
            {
                return PathVerdict::Malformed;
            }

            std::string decodedPath;
            if (!decodePathEscapes(rawPath, decodedPath))
            {
                return PathVerdict::Malformed;
            }

            if (isAbsoluteFormPath(decodedPath))
            {
                return PathVerdict::Forbidden;
            }

            // 解码之后才看得见的空字节（"%00"）
            if (decodedPath.find('\0') != std::string::npos)
            {
                return PathVerdict::Malformed;
            }

            if (decodedPath.find('\\') != std::string::npos || decodedPath.find(':') != std::string::npos)
            {
                return PathVerdict::Forbidden;
            }

            // 逐段检查并重组：剥掉前导 '/'，按 '/' 切开
            const std::string_view segmentsView(decodedPath.data() + 1, decodedPath.size() - 1);
            std::string_view       remainder = segmentsView;

            while (!remainder.empty())
            {
                const std::size_t      slashPosition  = remainder.find('/');
                const std::string_view currentSegment = remainder.substr(0, slashPosition);

                // '..' 一律拒绝：静态服务没有「往上走一级」的正当需求，
                // 而只要允许一段 '..'，后面无论怎么规范化都可能被符号链接再拐出去
                if (currentSegment == "..")
                {
                    return PathVerdict::Forbidden;
                }

                // '.' 是当前目录的别名，空段来自连续斜杠（"/a//b"）：两种都是「不改变指向」的写法，
                // 直接丢弃等价于归一化，拼出来的相对路径与原始语义一致，也不算攻击
                if (currentSegment == "." || currentSegment.empty())
                {
                    // 有意什么都不追加
                } else
                {
                    if (!relativeText.empty())
                    {
                        relativeText.push_back('/');
                    }
                    relativeText.append(currentSegment);
                }

                if (slashPosition == std::string_view::npos)
                {
                    break;
                }
                remainder = remainder.substr(slashPosition + 1);
            }

            // 清洗后什么都不剩（请求 "/"、"/."、"//." 一类）：不在此处决定，
            // 交给上层按「根目录本身」处理，避免把一个目录当文件读
            return PathVerdict::Accepted;
        }

        /**
         * @brief 判定候选路径是否仍然落在静态根目录之内
         *
         * @details 两侧都必须是 weakly_canonical 之后的路径：未归一化的路径里 ".."、"."、重复
         *          斜杠都能骗过纯字符串前缀比较，归一化后符号链接的逃逸也会暴露。用 relative 而非
         *          mismatch——后者只给一个迭代器，"/www" 与 "/wwwsecret" 这类只差一段名的情况判不出来。
         *
         * @param candidateFilePath 待判定的完整路径（已归一化）
         * @param rootDirectoryPath 静态根目录（已归一化）
         * @return true 候选路径在根目录之内（含等于根目录本身）
         * @return false 落在根目录之外，或两者位于不同盘符/根路径
         */
        bool isWithinRootDirectory(const std::filesystem::path &candidateFilePath, const std::filesystem::path &rootDirectoryPath)
        {
            // lexically_relative 是 path 的成员而不是自由函数，且是纯词法计算、不触碰文件系统，
            // 因此不存在「算不出来」的失败码；文件是否真的存在由调用方的 exists/is_regular_file 兜住
            const std::filesystem::path relativePath = candidateFilePath.lexically_relative(rootDirectoryPath);
            if (relativePath.empty())
            {
                // 两段路径完全相同：候选路径就是根目录本身
                return true;
            }
            if (relativePath.is_absolute())
            {
                // Windows 上「不同盘符」会算出绝对路径而不是相对路径，这条判据专门接住它
                return false;
            }

            // 结果里出现 ".." 就说明往上出去了，且 ".." 只可能出现在开头，取首段判定即可
            const std::filesystem::path firstSegment = *relativePath.begin();
            return firstSegment != "..";
        }

        /**
         * @brief 解析一段全数字文本为无符号整数
         * @details 必须整段消费，多出符号、空白或非数字字符一律判失败，不做部分解析。
         * @param text 待解析文本
         * @param value 输出：解析结果（失败时不被写入）
         * @return true 解析成功
         */
        bool parseUnsignedDecimal(const std::string_view text, std::uintmax_t &value)
        {
            if (text.empty())
            {
                return false;
            }
            std::uintmax_t parsedValue         = 0;
            const auto [endPointer, errorCode] = std::from_chars(text.data(), text.data() + text.size(), parsedValue);
            if (errorCode != std::errc() || endPointer != text.data() + text.size())
            {
                return false;
            }
            value = parsedValue;
            return true;
        }

        /// 强 ETag 文本的字节上限：引号 + 十六进制大小（最多 16）+ 连字符 + 十六进制修改秒（负数带符号，最多 17）+ 引号
        constexpr std::size_t kMaximumEtagTextBytes = 36U;

        /**
         * @brief 按「文件大小 + 修改时间整秒」构造强 ETag
         * @param fileSize 文件字节数
         * @param lastWriteSeconds 文件修改时间，自 Unix 纪元起的秒数
         * @param buffer 输出缓冲，容量恰为 kMaximumEtagTextBytes
         * @return 形如 "\"1a-5f2c3d4e\"" 的带双引号标签；大小或修改时间任一变化都会改变它
         */
        std::string_view makeStrongEtag(const std::uintmax_t fileSize, const std::int64_t lastWriteSeconds, std::array<char, kMaximumEtagTextBytes> &buffer)
        {
            // 写法与旧的「两段 to_chars 再拼进 string」逐字节一致：同一个十六进制基数、同一个字段
            // 次序，只是目标换成了调用方的缓冲。这段文本只活到 setHeader 把内容拷走，按值交出就是
            // 每个静态请求一份超出小串内联缓冲的堆分配
            std::size_t cursor = 0;
            buffer[cursor++]   = '"';
            // 缓冲按上限算足（1+16+1+17+1 = 36），仍按剩余区间逐段交出：to_chars 写不下只会返回
            // 错误而不越界
            const auto sizeResult = std::to_chars(buffer.data() + cursor, buffer.data() + buffer.size(), fileSize, 16);
            cursor                = static_cast<std::size_t>(sizeResult.ptr - buffer.data());
            buffer[cursor++]      = '-';
            const auto timeResult = std::to_chars(buffer.data() + cursor, buffer.data() + buffer.size(), lastWriteSeconds, 16);
            cursor                = static_cast<std::size_t>(timeResult.ptr - buffer.data());
            buffer[cursor++]      = '"';
            return std::string_view(buffer.data(), cursor);
        }

        /**
         * @brief 判断 If-None-Match 的值是否命中本资源的 ETag
         * @details 值为 "*" 或列表里任一标签（弱比较）与本资源标签相同即命中（RFC 9110 §13.1.2）。
         * @param listValue If-None-Match 头原文
         * @param entityTag 本资源当前 ETag
         * @return true 命中
         */
        bool entityTagListMatches(const std::string_view listValue, const std::string_view entityTag)
        {
            return weakEntityTagListMatches(listValue, entityTag);
        }

        /**
         * @brief 判断 If-Match 的前提是否**不成立**（成立时返回 false，让请求照常走）
         * @details §13.2.2 的 MUST：所选表示与给定标签不符即回 412。这里用强比较而不是复用它上面
         *          那句弱比较——两者区别是规范规定的：`W/"x"` 永不强匹配，而 `*` 作为列表一项时
         *          也不代表「任意资源」，只有整值为 `*` 才表示「只要它还在」。
         *          头场不写或写成空值时不参与判定（§13.2：空列表在语法上非法，按不带处理更安全）。
         * @param request 请求对象
         * @param entityTag 本资源当前 ETag
         * @return true 条件不成立，应当回 412
         */
        bool preconditionFails(const HttpRequest &request, const std::string_view entityTag)
        {
            const std::optional<std::string> ifMatch = request.getHeader("if-match");
            if (!ifMatch.has_value() || trimOptionalWhitespace(*ifMatch).empty())
            {
                return false;
            }
            // 走到这里说明文件已经打开成功（存在性成立），因此 * 命中
            return !strongEntityTagListMatches(*ifMatch, entityTag, true);
        }

        /**
         * @brief 判断本次请求的条件头部是否命中「未修改」
         * @details If-None-Match 在场时按 RFC 9110 §13.1.3 忽略 If-Modified-Since；
         *          两个日期都只精确到秒，因此按整秒比较。
         * @param request 请求对象
         * @param entityTag 本资源当前 ETag
         * @param lastWriteSeconds 本资源 Last-Modified 的整秒值
         * @return true 应当回 304
         */
        bool isNotModified(const HttpRequest &request, const std::string_view entityTag, const std::int64_t lastWriteSeconds)
        {
            const std::optional<std::string> ifNoneMatch = request.getHeader("if-none-match");
            if (ifNoneMatch.has_value())
            {
                return entityTagListMatches(*ifNoneMatch, entityTag);
            }

            const std::optional<std::string> ifModifiedSince = request.getHeader("if-modified-since");
            if (!ifModifiedSince.has_value())
            {
                return false;
            }

            const std::optional<std::chrono::system_clock::time_point> parsedSince = parseHttpDate(trimOptionalWhitespace(*ifModifiedSince));
            if (!parsedSince.has_value())
            {
                // 日期解析不出来就不算条件命中：宁可回完整表示，也不误判成「没变」
                return false;
            }
            const std::int64_t sinceSeconds = std::chrono::duration_cast<std::chrono::seconds>(parsedSince->time_since_epoch()).count();
            return lastWriteSeconds <= sinceSeconds;
        }

        /**
         * @brief 判断本次请求是否满足「应用 Range」的前提（If-Range 校验）
         * @details If-Range 缺省即放行；在场时其值必须与本资源 ETag 逐字相同（强比较），
         *          或解析为日期且与 Last-Modified 相等（RFC 9110 §13.1.5）：两者都不成立时
         *          忽略 Range，按 200 下发完整表示。
         * @param request 请求对象
         * @param entityTag 本资源当前 ETag
         * @param lastWriteSeconds 本资源 Last-Modified 的整秒值
         * @return true 可以按 Range 处理
         */
        bool isRangeApplicable(const HttpRequest &request, const std::string_view entityTag, const std::int64_t lastWriteSeconds)
        {
            const std::optional<std::string> ifRange = request.getHeader("if-range");
            if (!ifRange.has_value())
            {
                return true;
            }

            const std::string_view ifRangeValue = trimOptionalWhitespace(*ifRange);
            // 本资源下发的是强 ETag，If-Range 必须逐字相同；弱标签不应用于 Range
            if (ifRangeValue == entityTag)
            {
                return true;
            }

            const std::optional<std::chrono::system_clock::time_point> parsedDate = parseHttpDate(ifRangeValue);
            if (!parsedDate.has_value())
            {
                return false;
            }
            const std::int64_t ifRangeSeconds = std::chrono::duration_cast<std::chrono::seconds>(parsedDate->time_since_epoch()).count();
            // 日期验证器走「相等」而不是「不晚于」：If-Range 问的是「你手上那份还是不是我现在这份」，
            // 对端的日期再晚也不代表表示没变——文件被换回更早的版本（恢复备份、时钟回拨）时，
            // 按「不晚于」放行就会把另一份表示的字节段拼进对端的缓存，正是 Range 校验要防的事
            return lastWriteSeconds == ifRangeSeconds;
        }

        /// 字节区间的解析结论
        enum class RangeVerdict
        {
            Ignored,         ///< 头不存在、语法非法、区间太多或拼起来太大：忽略该头，按 200 全量返回
            Unsatisfiable,   ///< 起点超出表示长度或后缀为 0：应答 416
            Satisfiable,     ///< 合并后只剩一个可用闭区间：单段 206
            MultiSatisfiable ///< 合并后仍有多个区间：206 带 multipart/byteranges
        };

        /**
         * @brief 单个字节区间的解析结果
         */
        struct ByteRange
        {
            std::uintmax_t start{0}; ///< 区间起始偏移（含）
            std::uintmax_t end{0};   ///< 区间结束偏移（含）
        };

        /// 一条 Range 合并后最多接受几个区间：再多就不是「补几段」而是用一条请求榨内存
        constexpr std::size_t kMaximumRequestedByteRangeCount = 8;

        /// 多区间拼成一包正文的字节上界；越过它就整条 Range 当没看见（回 200 全量）
        constexpr std::uintmax_t kMaximumMultiRangeBodyBytes = 8ull * 1024 * 1024;

        /// multipart/byteranges 的分隔符（RFC 2046 §5.1.1 的 bchars）
        /**
         * @details 固定而不随机：同一请求的两次应答要逐字一致，日志与用例才钉得住。
         *          代价是正文里可能撞上它——那一判的处理是回全量，不换边界重试（换边界等于
         *          同一请求两次给出不同报文，比多发一段全量更难解释）
         */
        constexpr std::string_view kByterangesBoundary = "AsynGyanisByteranges";

        /**
         * @brief 解析 byte-range-spec：'bytes=' 之后、按逗号切开的其中一段
         * @details `bytes=start-end`、`bytes=start-`、`bytes=-suffix` 三种形态都支持，end 超出表示
         *          末尾时截到末尾（RFC 9110 §14.1.1）。返回 Ignored 表示这一段写法本身不合法，
         *          按规范整条 Range 都该被忽略；Unsatisfiable 只表示这一段越界，别的段仍可满足。
         * @param spec 这一段区间原文（不含单位与前后的逗号）
         * @param representationSize 当前表示的字节数
         * @param byteRange 输出：可满足的闭区间（仅返回 Satisfiable 时有效）
         * @return RangeVerdict 这一段的结论（只会是 Ignored/Unsatisfiable/Satisfiable）
         */
        RangeVerdict parseOneByteRange(const std::string_view spec, const std::uintmax_t representationSize, ByteRange &byteRange)
        {
            byteRange = ByteRange{};

            const std::string_view rangeSpec = trimOptionalWhitespace(spec);
            if (rangeSpec.empty())
            {
                return RangeVerdict::Ignored;
            }
            if (rangeSpec.front() == '-')
            {
                // 后缀形态：取末尾 suffixLength 个字节
                std::uintmax_t suffixLength = 0;
                if (!parseUnsignedDecimal(rangeSpec.substr(1), suffixLength))
                {
                    return RangeVerdict::Ignored;
                }
                if (suffixLength == 0 || representationSize == 0)
                {
                    // 0 字节后缀与空表示都不存在可满足的区间；判序必须在减法之前，
                    // 否则 representationSize - 1 会绕成一个巨大的末端
                    return RangeVerdict::Unsatisfiable;
                }
                byteRange.start = (suffixLength >= representationSize) ? 0 : representationSize - suffixLength;
                byteRange.end   = representationSize - 1;
                return RangeVerdict::Satisfiable;
            }

            const std::size_t dashPosition = rangeSpec.find('-');
            if (dashPosition == std::string_view::npos)
            {
                return RangeVerdict::Ignored;
            }

            std::uintmax_t startValue = 0;
            if (!parseUnsignedDecimal(rangeSpec.substr(0, dashPosition), startValue))
            {
                return RangeVerdict::Ignored;
            }
            if (startValue >= representationSize)
            {
                // 起点已在表示之外：这一段不可满足，别的段照旧算
                return RangeVerdict::Unsatisfiable;
            }

            const std::string_view endText  = rangeSpec.substr(dashPosition + 1);
            std::uintmax_t         endValue = representationSize - 1;
            if (!endText.empty())
            {
                if (!parseUnsignedDecimal(endText, endValue))
                {
                    return RangeVerdict::Ignored;
                }
                // first-byte-pos > last-byte-pos 的区间非法（RFC 9110 §14.1.1），整条忽略
                if (startValue > endValue)
                {
                    return RangeVerdict::Ignored;
                }
                if (endValue > representationSize - 1)
                {
                    endValue = representationSize - 1;
                }
            }

            byteRange.start = startValue;
            byteRange.end   = endValue;
            return RangeVerdict::Satisfiable;
        }

        /**
         * @brief 解析 Range 头里的一个或多个字节区间，并把重叠与相邻的段合并
         *
         * @details 合并是 RFC 9110 §14.2 明给的自由（「服务端可以把重叠或相邻的区间合成一段发」），
         *          这里用它换两样东西：`0-4,3-6` 这种写法不会发出两份重叠字节，以及
         *          「区间数」这个上界判的是真正要发的段数而不是对端写了几段。
         *          两道刻意的退回：段数越过上限、或合并后总字节越过上限时**整条 Range 当没看见**
         *          （回 200 全量）——与其用一条请求换走数倍正文内存，不如让对端拿完整表示。
         *
         * @param headerValue Range 头原文
         * @param representationSize 当前表示的字节数
         * @param ranges 输出：按起点升序、互不重叠也不相邻的闭区间（仅在返回 Satisfiable/MultiSatisfiable 时非空）
         * @return RangeVerdict 整体结论
         */
        RangeVerdict parseByteRanges(const std::string_view headerValue, const std::uintmax_t representationSize, std::vector<ByteRange> &ranges)
        {
            ranges.clear();

            const std::size_t equalsPosition = headerValue.find('=');
            if (equalsPosition == std::string_view::npos)
            {
                return RangeVerdict::Ignored;
            }
            // 区间单位大小写不敏感（RFC 9110 §14.1）
            if (!equalsIgnoringCase(trimOptionalWhitespace(headerValue.substr(0, equalsPosition)), "bytes"))
            {
                return RangeVerdict::Ignored;
            }

            const std::string_view rangeSpec = trimOptionalWhitespace(headerValue.substr(equalsPosition + 1));
            if (rangeSpec.empty())
            {
                return RangeVerdict::Ignored;
            }

            std::vector<ByteRange> parsed{};
            bool                   hasUnsatisfiableSegment = false;
            std::size_t            segmentStart            = 0;
            while (segmentStart <= rangeSpec.size())
            {
                const std::size_t  commaPosition = rangeSpec.find(',', segmentStart);
                const std::size_t  segmentEnd    = commaPosition == std::string_view::npos ? rangeSpec.size() : commaPosition;
                ByteRange          one{};
                const RangeVerdict segmentVerdict = parseOneByteRange(rangeSpec.substr(segmentStart, segmentEnd - segmentStart), representationSize, one);
                if (segmentVerdict == RangeVerdict::Ignored)
                {
                    // 有一段写法不合法：整条 byte-range-set 就非法，按 RFC 忽略（回 200 全量）
                    ranges.clear();
                    return RangeVerdict::Ignored;
                }
                if (segmentVerdict == RangeVerdict::Unsatisfiable)
                {
                    hasUnsatisfiableSegment = true;
                } else
                {
                    parsed.push_back(one);
                }
                if (commaPosition == std::string_view::npos)
                {
                    break;
                }
                segmentStart = commaPosition + 1;
            }

            if (parsed.empty())
            {
                // 全都越界才是 416；一段都没有（空表）在上面已被 Ignored 分支挡掉
                return hasUnsatisfiableSegment ? RangeVerdict::Unsatisfiable : RangeVerdict::Ignored;
            }

            // 按起点排序后合并「重叠或紧邻」的段：合并后的段数才是要真发出去的段数，
            // 两道上限都按它判（对端写了几段不算）
            std::ranges::sort(parsed, [](const ByteRange &left, const ByteRange &right) { return left.start < right.start; });
            ranges.push_back(parsed.front());
            std::uintmax_t totalBytes = parsed.front().end - parsed.front().start + 1;
            for (std::size_t index = 1; index < parsed.size(); ++index)
            {
                ByteRange &tail = ranges.back();
                // end 是「含」的闭区间末端：+1 后与下一段起点相接即视为可合并（紧邻的两段合成一段发，
                // 少一个 MIME 段也少一份头部）。+1 之前先判是否已到表示末尾，避免末端绕回
                const bool isAdjacentOrOverlapping = parsed[index].start <= tail.end || parsed[index].start == tail.end + 1;
                if (isAdjacentOrOverlapping)
                {
                    if (parsed[index].end > tail.end)
                    {
                        totalBytes += parsed[index].end - tail.end;
                        tail.end = parsed[index].end;
                    }
                    continue;
                }
                totalBytes += parsed[index].end - parsed[index].start + 1;
                ranges.push_back(parsed[index]);
            }

            // 两道退回都是「宁可回全量」：段数或总字节越界时不陪对端做内存游戏
            if (ranges.size() > kMaximumRequestedByteRangeCount || (ranges.size() > 1 && totalBytes > kMaximumMultiRangeBodyBytes))
            {
                ranges.clear();
                return RangeVerdict::Ignored;
            }
            return ranges.size() == 1 ? RangeVerdict::Satisfiable : RangeVerdict::MultiSatisfiable;
        }

        /// 一份目录列表最多列出的条目数：更大的目录不该把整棵树的字节名塞进一条响应
        constexpr std::size_t kMaximumListedDirectoryEntries = 1000;

        /**
         * @brief 按 HTML 文本/属性节点的要求转义一段文本
         * @details 文件名里出现 `<`、`&`、引号是完全合法的，不转义就等于让对端往本服务生成的
         *          页面里写标签（存储型 XSS 的经典入口：文件名由上传方决定）
         * @param output 追加目标
         * @param text 待转义文本
         */
        void appendHtmlEscapedText(std::string &output, const std::string_view text)
        {
            for (const char character: text)
            {
                switch (character)
                {
                    case '<':
                        output.append("&lt;");
                        break;
                    case '>':
                        output.append("&gt;");
                        break;
                    case '&':
                        output.append("&amp;");
                        break;
                    case '"':
                        output.append("&quot;");
                        break;
                    case '\'':
                        output.append("&#39;");
                        break;
                    default:
                        output.push_back(character);
                }
            }
        }

        /**
         * @brief 把文件名收成能放进 href 的路径段
         * @details 保留字符、控制字符、空格与 `%` 一律百分号编码——`%` 必须先编码，否则名字里
         *          本来就有的「%41」会被浏览器解成字母 A，链接指到另一个条目上去。
         *          非 ASCII 字节原样留着：页面已声明 UTF-8，浏览器自己折算。
         * @param output 追加目标
         * @param name 文件名原文（UTF-8）
         */
        void appendUrlEncodedPathSegment(std::string &output, const std::string_view name)
        {
            static constexpr std::string_view kUnreservedCharacters = "-._~";
            static constexpr char             kHexDigits[]          = "0123456789ABCDEF";
            for (const char character: name)
            {
                const bool isAlphaNumeric = (character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
                if (isAlphaNumeric || kUnreservedCharacters.find(character) != std::string_view::npos)
                {
                    output.push_back(character);
                    continue;
                }
                const unsigned char byte = static_cast<unsigned char>(character);
                output.push_back('%');
                output.push_back(kHexDigits[byte >> 4U]);
                output.push_back(kHexDigits[byte & 0x0FU]);
            }
        }

        /**
         * @brief 一个待列出的目录条目
         */
        struct DirectoryEntry
        {
            std::string    name;                ///< 文件名原文（UTF-8）
            bool           isDirectory{false};  ///< 是否目录（列表里带尾斜杠并排在文件之前）
            std::uintmax_t sizeBytes{0};        ///< 字节数；目录按 0 报，不替它算子树大小
            std::int64_t   lastWriteSeconds{0}; ///< 最后修改时间的 Unix 秒
        };

        /**
         * @brief 生成一份 HTML 目录列表并写进响应
         *
         * @details 三条刻意的口径，都写进用例：
         *          @li 正文现读生成，因此**不给验证器**且带 `cache-control: no-store`——一份被缓存的
         *              列表会在目录变化之后继续宣称「这里有这些文件」，而条件请求那套（ETag/Last-Modified）
         *              在这里没有可信的来源；
         *          @li 忽略 Range：列表不是一个长度稳定的表示；
         *          @li 条目数到上界就停，并在末尾如实写明还有多少项没列出，不做半截列表装作是全的。
         *
         * @param directory 要列出的目录（调用方已确认它在静态根之内）
         * @param requestPath 请求路径原文，用于标题、条目链接的前缀与「上一级」的判定
         * @param response 待填的响应
         */
        void renderDirectoryListing(const std::filesystem::path &directory, const std::string_view requestPath, HttpResponse &response)
        {
            std::vector<DirectoryEntry> entries;
            entries.reserve(kMaximumListedDirectoryEntries);

            std::error_code                     iterateError;
            std::filesystem::directory_iterator iterator(directory, std::filesystem::directory_options::skip_permission_denied, iterateError);
            if (iterateError)
            {
                // 打不开目录（权限、被删除、或它压根不是目录）：与「没有这个资源」同形状，
                // 不把文件系统错误细节交给探测者
                response.setStatus(404);
                response.setBody("Not Found");
                response.setHeader("content-type", "text/plain");
                return;
            }

            std::size_t observedEntryCount = 0;
            for (const std::filesystem::directory_entry &candidate: iterator)
            {
                ++observedEntryCount;
                if (entries.size() >= kMaximumListedDirectoryEntries)
                {
                    continue; // 到界之后只数不列，末尾那句「还有 N 项未列出」靠这个计数
                }

                std::error_code typeError;
                const bool      isDirectory = candidate.is_directory(typeError);
                if (typeError)
                {
                    continue; // 迭代之后条目又被删掉：跳过这一条，不交出半截信息
                }

                // 大小与修改时间走平台那一份查询：它同时给出正确的 UTF-8 刻度时间换算，
                // 而 std::filesystem::last_write_time 的 file_time 在两平台不是一个坐标系
                const std::optional<Platform::FileBasicInfo> entryInfo = Platform::queryFileBasicInfo(candidate.path());
                if (!entryInfo.has_value())
                {
                    continue;
                }

                DirectoryEntry entry;
                // 名字按 UTF-8 出串：Windows 上 path::string() 走本地代码页，非 ASCII 的名字
                // 会列成另一串，链接点了就找不到（与静态目录配置那套 UTF-8 刻度同一个坑）
                entry.name             = Platform::FileSystem::utf8FromPath(candidate.path().filename());
                entry.isDirectory      = isDirectory;
                entry.sizeBytes        = isDirectory ? 0U : entryInfo->sizeBytes;
                entry.lastWriteSeconds = entryInfo->lastWriteSeconds;
                entries.push_back(std::move(entry));
            }

            // 目录排在文件前，各自按名字排序：翻列表的人先看到结构再看到文件
            std::ranges::sort(entries,
                              [](const DirectoryEntry &left, const DirectoryEntry &right)
                              {
                                  if (left.isDirectory != right.isDirectory)
                                  {
                                      return left.isDirectory;
                                  }
                                  return left.name < right.name;
                              });

            std::string body;
            body.reserve(512U + entries.size() * 96U);
            body.append("<!DOCTYPE html>\r\n<html lang=\"zh-CN\"><head><meta charset=\"utf-8\"><title>Index of ");
            appendHtmlEscapedText(body, requestPath);
            body.append("</title></head>\r\n<body><h1>Index of ");
            appendHtmlEscapedText(body, requestPath);
            body.append("</h1>\r\n<ul>\r\n");

            // 链接一律用绝对路径：同一棵目录既可能被挂在 /sub（没有尾斜杠）下，也可能挂在 /sub/ 下，
            // 相对链接在前者上会全部指到父目录去——那比列错一个名字更难查
            std::string basePath(requestPath);
            if (basePath.empty() || basePath.back() != '/')
            {
                basePath.push_back('/');
            }

            // 根目录没有「上一级」可去。求父级是在 basePath（必以 '/' 收尾）上剥掉最后一段：
            // "/sub/" → "/"，"/a/b/" → "/a/"
            if (basePath.size() > 1U)
            {
                const std::string parentPath = basePath.substr(0, basePath.find_last_of('/', basePath.size() - 2) + 1U);
                body.append("<li><a href=\"");
                appendHtmlEscapedText(body, parentPath);
                body.append("\">../</a></li>\r\n");
            }

            for (const DirectoryEntry &entry: entries)
            {
                body.append("<li><a href=\"");
                body.append(basePath);
                appendUrlEncodedPathSegment(body, entry.name);
                if (entry.isDirectory)
                {
                    body.append("/\">");
                    appendHtmlEscapedText(body, entry.name);
                    body.append("/</a></li>\r\n");
                    continue;
                }
                body.append("\">");
                appendHtmlEscapedText(body, entry.name);
                body.append("</a> ");
                body.append(std::to_string(entry.sizeBytes));
                body.append(" B");
                std::array<char, kHttpDateTextLength> dateBuffer{};
                body.append(" ");
                body.append(formatHttpDate(timePointFromUnixSeconds(entry.lastWriteSeconds), dateBuffer));
                body.append("</li>\r\n");
            }

            if (observedEntryCount > entries.size())
            {
                body.append("<li>…另有 ");
                body.append(std::to_string(observedEntryCount - entries.size()));
                body.append(" 项未列出</li>\r\n");
            }
            body.append("</ul>\r\n</body></html>\r\n");

            response.setStatus(200);
            response.setHeader("content-type", "text/html; charset=utf-8");
            response.setHeader("cache-control", "no-store");
            response.setHeader("accept-ranges", "none");
            response.setBody(std::move(body));
        }

        /**
         * @brief 把多区间的正文拼成一份 multipart/byteranges 并写成一条 206
         *
         * @details 状态码与 content-type 只在**全部正文就位之后**才写：半途失败（文件被删或被换成
         *          另一个版本、正文里撞上了分隔符）时响应还是一个干净的空对象，调用方可以改判
         *          「这条 Range 没接」回 200 全量，而不是发出一份自相矛盾的 206。
         *          零拷贝在这里换不成：正文是多段拼出来的，每段读完都要拷进那一块缓冲——代价由
         *          `kMaximumMultiRangeBodyBytes` 封顶，仍比让对端重取整份文件便宜。
         *
         * @param candidatePath 已确认落在静态根之内的文件路径
         * @param ranges 合并后的区间序列（起点升序、互不重叠也不相邻，长度 ≥ 2）
         * @param fileSize 表示的字节数，只用于每段的 Content-Range 总长
         * @param mimeType 这份文件的媒体类型，逐段带上（RFC 9110 §14.6）
         * @param statInfo 建立验证器那次查询拿到的文件身份，逐段复核
         * @param isHeadRequest 是否为 HEAD：只报「GET 会发多大」，一段正文都不必读
         * @param response 待填的响应
         * @return true 已写成 206；false 表示没写成且响应未被改动
         */
        Core::Task<bool> writeMultipartByteRangesBody(const std::filesystem::path &candidatePath, const std::vector<ByteRange> &ranges, const std::uintmax_t fileSize,
                                                      const std::string_view mimeType, const Platform::FileBasicInfo &statInfo, const bool isHeadRequest, HttpResponse &response)
        {
            const std::string delimiter = "--" + std::string(kByterangesBoundary);

            // 先把每段的头部文本拼出来并算总长：HEAD 只要这个长度，GET 按它填满正文缓冲
            std::vector<std::string> partHeaders;
            partHeaders.reserve(ranges.size());
            std::size_t totalBytes = 0;
            for (const ByteRange &range: ranges)
            {
                std::string partHeader;
                partHeader.reserve(delimiter.size() + mimeType.size() + 96U);
                partHeader.append(delimiter);
                partHeader.append("\r\n");
                partHeader.append("content-type: ");
                partHeader.append(mimeType);
                partHeader.append("\r\n");
                partHeader.append("content-range: bytes ");
                partHeader.append(std::to_string(range.start));
                partHeader.push_back('-');
                partHeader.append(std::to_string(range.end));
                partHeader.push_back('/');
                partHeader.append(std::to_string(fileSize));
                partHeader.append("\r\n\r\n");
                // 段与段之间的界线是「CRLF + 分隔符行」：那对 CRLF 归在这一段的末尾，
                // 不属于正文（RFC 2046 §5.1），所以每段实际占的长度还要再加 2 字节
                totalBytes += partHeader.size() + static_cast<std::size_t>(range.end - range.start + 1) + 2U;
                partHeaders.push_back(std::move(partHeader));
            }
            const std::string closingDelimiter = delimiter + "--\r\n";
            totalBytes += closingDelimiter.size();

            if (isHeadRequest)
            {
                response.setStatus(206);
                response.setHeader("content-type", "multipart/byteranges; boundary=" + std::string(kByterangesBoundary));
                response.setHeader("content-length", std::to_string(totalBytes));
                co_return true;
            }

            std::string &body     = response.prepareBodyBuffer(totalBytes);
            std::size_t  position = 0;
            std::string  scratch; ///< 每段先读到这里再拷进正文：容量跨段复用，不再逐段要堆
            for (std::size_t index = 0; index < ranges.size(); ++index)
            {
                const ByteRange  &segment       = ranges[index];
                const std::size_t segmentLength = static_cast<std::size_t>(segment.end - segment.start + 1);
                std::char_traits<char>::copy(body.data() + position, partHeaders[index].data(), partHeaders[index].size());
                position += partHeaders[index].size();

                const std::size_t                                 dataOffset = position;
                Platform::FileBasicInfo                           openedAs{};
                const std::expected<std::size_t, std::error_code> readResult =
                        Platform::readFileContentsInto(candidatePath, static_cast<std::size_t>(segment.start), segmentLength, scratch, &openedAs);
                if (!readResult.has_value() || *readResult != segmentLength)
                {
                    // 读不出来或短读：文件在两次请求之间被截断/换掉，不能发半份区间
                    response.setBody(std::string{});
                    co_return false;
                }
                if (openedAs.sizeBytes != fileSize || openedAs.lastWriteSeconds != statInfo.lastWriteSeconds || openedAs.identityTag != statInfo.identityTag)
                {
                    // 验证器描述的是「建立它那次查询」的那个版本，正文却来自另一个对象：
                    // 与单区间那一条同样的判据，宁可回全量也不把两个版本的字节拼给对端
                    response.setBody(std::string{});
                    co_return false;
                }
                if (scratch.find("\r\n" + delimiter) != std::string::npos)
                {
                    // 正文里出现了分隔符：发出去会被对端切错段，改判回全量（见 kByterangesBoundary 的取舍）
                    response.setBody(std::string{});
                    co_return false;
                }
                std::char_traits<char>::copy(body.data() + dataOffset, scratch.data(), segmentLength);
                position += segmentLength;
                // 段末尾那对 CRLF 与下一行的 "--boundary" 一起构成分隔符（RFC 2046 §5.1）
                std::char_traits<char>::copy(body.data() + position, "\r\n", 2U);
                position += 2U;
            }
            std::char_traits<char>::copy(body.data() + position, closingDelimiter.data(), closingDelimiter.size());

            response.setStatus(206);
            response.setHeader("content-type", "multipart/byteranges; boundary=" + std::string(kByterangesBoundary));
            co_return true;
        }

        /**
         * @brief 把一条请求当作静态文件请求处理，填充响应
         * @details 除路径清洗与文件查找外，还负责缓存验证（ETag / Last-Modified 与 304）
         *          与单区间 Range（206/416）；只读取方法、路径与条件请求/区间头部。
         * @param request 请求对象
         * @param response 响应对象，由路由器保证进入本函数时是干净的
         * @param settings 静态文件配置（按值捕获的共享指针，保证处理函数不依赖服务器生命周期）
         * @return Core::Task<> 协程；本函数内部不抛异常，所有失败都落成 4xx/5xx
         */
        Core::Task<> serveStaticFileRequest(HttpRequest &request, HttpResponse &response, const std::shared_ptr<StaticFileSettings> settings)
        {
            const HttpMethod requestMethod = request.method();

            // 静态目录只读：非 GET/HEAD 一律 405 并如实声明支持的方法，不落到后面的「文件不存在」
            if (requestMethod != HttpMethod::GET && requestMethod != HttpMethod::HEAD)
            {
                response.setStatus(405);
                response.setBody("Method Not Allowed");
                response.setHeader("content-type", "text/plain");
                response.setHeader("allow", "GET, HEAD");
                co_return;
            }

            // 未启用（含根目录规范化失败）时按「无此资源」处理：不把「服务器配置有问题」告诉客户端
            if (settings == nullptr || !settings->isEnabled)
            {
                response.setStatus(404);
                response.setBody("Not Found");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            std::string relativeText;
            // 读 path() 而不是通配捕获的 param("*")：兜底路由的模式就是裸 "*"，两者内容只差一个
            // 前导 '/'，而下面的清洗函数要求路径以 '/' 开头，用 path() 既少一次拼接也少一处歧义
            const PathVerdict verdict = sanitizeRequestPath(request.path(), relativeText);
            if (verdict == PathVerdict::Malformed)
            {
                // 400：路径本身畸形，客户端改对了才有下一次
                response.setStatus(400);
                response.setBody("Bad Request: Malformed Path");
                response.setHeader("content-type", "text/plain");
                co_return;
            }
            if (verdict == PathVerdict::Forbidden)
            {
                // 403：形态合法但意图越权，明确告知是被拒绝而不是找不到
                response.setStatus(403);
                response.setBody("Forbidden");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            // 请求打到静态根本身：开了目录列表就列出来，没开（默认）按 404 处理
            if (relativeText.empty())
            {
                if (!settings->listingEnabled)
                {
                    response.setStatus(404);
                    response.setBody("Not Found");
                    response.setHeader("content-type", "text/plain");
                    co_return;
                }
                renderDirectoryListing(settings->rootDirectory, request.path(), response);
                co_return;
            }

            std::error_code pathError;
            // URI 解码给出的是 UTF-8 文本，先按 UTF-8 解成路径对象再拼接：直接把窄串交给 path，Windows
            // 会按本地代码页解释这段字节，非 ASCII 的名字因此永远指向另一个目录项（一律 404）
            const std::filesystem::path candidatePath = std::filesystem::weakly_canonical(settings->rootDirectory / Platform::FileSystem::pathFromUtf8(relativeText), pathError);
            if (pathError)
            {
                // 归一化失败多因路径过长、字符集不支持或中途权限不足：与「不存在」同权重，回 404
                response.setStatus(404);
                response.setBody("Not Found");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            // 归一化之后的第二道包含判定：拦住 ..\、符号链接、大小写与平台分隔符的一切花样
            if (!isWithinRootDirectory(candidatePath, settings->rootDirectory))
            {
                response.setStatus(403);
                response.setBody("Forbidden");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            // 元数据（类型/大小/修改时间）每请求现读，但只读一次：一次底层查询同时给出三样，
            // 逐项取值与 std::filesystem 的那三个函数一致。随后 mmap 的文件首触仍会产生缺页，
            // 这是静态文件服务的固有代价（nginx 同形态）。要连这份现读一起消掉得加一层
            // 「路径 → 元数据」缓存并配失效策略（交给 file watcher 或 TTL），实测表明它成为瓶颈
            // 之前不做：缓存失效写错会把「文件更新后仍旧 ETag」变成真缺陷
            const std::optional<Platform::FileBasicInfo> fileBasicInfo = Platform::queryFileBasicInfo(candidatePath);
            if (!fileBasicInfo.has_value())
            {
                // 查不到元数据（不存在、权限不足或查询本身失败）：按「没有这个资源」处理
                response.setStatus(404);
                response.setBody("Not Found");
                response.setHeader("content-type", "text/plain");
                co_return;
            }
            if (!fileBasicInfo->isRegularFile)
            {
                // 目录与设备文件都到不了「把文件正文发出去」那一步。只有显式开了目录列表才多问一次
                // 「这是目录吗」：列表关着时（默认）探测者拿到的仍是一条普通 404，不泄露目录结构
                std::error_code directoryError;
                if (settings->listingEnabled && std::filesystem::is_directory(candidatePath, directoryError) && !directoryError)
                {
                    renderDirectoryListing(candidatePath, request.path(), response);
                    co_return;
                }
                response.setStatus(404);
                response.setBody("Not Found");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            const std::uintmax_t fileSize = fileBasicInfo->sizeBytes;

            // 内存保护：超限的大文件不映射，也不给半截正文
            if (fileSize > kMaximumStaticFileSize)
            {
                response.setStatus(413);
                response.setBody("Payload Too Large");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            // 验证器：ETag 由「大小 + 修改时间整秒」构出，Last-Modified 由同一个整秒格式化而来。
            // 三样取自同一次查询，因此大小与修改时间必然描述同一个版本；分三次查时中间被改过
            // 就会拼出一对来自不同版本的验证器，那才是原先两处 500 分支想挡的东西
            const std::int64_t lastWriteSeconds = fileBasicInfo->lastWriteSeconds;
            // 两份验证器文本都落在栈上：它们只活到 setHeader 把内容拷进响应的头部存储为止，
            // 按 std::string 交回就是每请求两次超出内联缓冲的堆分配（ETag 最长 36、日期定长 29）
            std::array<char, kMaximumEtagTextBytes> etagBuffer{};
            const std::string_view                  entityTagText = makeStrongEtag(fileSize, lastWriteSeconds, etagBuffer);
            std::array<char, kHttpDateTextLength>   lastModifiedBuffer{};
            const std::string_view                  lastModifiedText = formatHttpDate(timePointFromUnixSeconds(lastWriteSeconds), lastModifiedBuffer);
            // MIME 只看最后一段扩展名，因此只把扩展名按 UTF-8 出串：整条路径的文本要一次堆分配，
            // 而扩展名短到能留在小串内联里。两条都不走 path::string()——Windows 上它按本地代码页出串，
            // 代码页装不下的名字会在这里抛出，而这条正站在每个静态请求的路上
            // 查表返回的是静态字面量，直接交给 setHeader 的 string_view 形参；再拷一份进 std::string
            // 等于把「application/octet-stream」这种超出内联的长度白付一次堆分配
            const char *const mimeType = FileSender::contentTypeForFile(Platform::FileSystem::utf8FromPath(candidatePath.extension()));

            // 缓存验证器与 Cache-Control 只在 200/206/304 上写一次，避免三处各写一遍而漂移
            const auto appendCacheHeaders = [&response, &entityTagText, &lastModifiedText, &settings]()
            {
                response.setHeader("etag", entityTagText);
                response.setHeader("last-modified", lastModifiedText);
                if (settings->cacheControl.has_value())
                {
                    response.setHeader("cache-control", *settings->cacheControl);
                }
            };

            const bool isHeadRequest = (requestMethod == HttpMethod::HEAD);

            // If-Match 排在「未修改」判定之前：两个条件同时在场时要**同时**成立（§13.2.2），
            // 先判 If-Match 才能把「前提不成立」与「没变化」这两种答复分清楚
            if (preconditionFails(request, entityTagText))
            {
                response.setStatus(412);
                response.setBody("Precondition Failed");
                static_cast<void>(response.setHeader("content-type", "text/plain"));
                co_return;
            }

            // 条件请求：命中验证器即 304，无正文，也不必打开文件映射
            if (isNotModified(request, entityTagText, lastWriteSeconds))
            {
                response.setStatus(304);
                appendCacheHeaders();
                // 304 里的 content-length 只允许取「同一请求的 200 会发出的正文长度」（RFC 9112 §6.2），
                // 这里正是唯一知道那个长度的位置（正文已被清空，自动补缺只会补出 0——那是禁止的取值）
                response.setHeader("content-length", std::to_string(fileSize));
                co_return;
            }

            // Range：仅在 If-Range 放行时解析，不放行时按 200 全量
            std::vector<ByteRange>           byteRanges{};
            ByteRange                        byteRange{};
            RangeVerdict                     rangeVerdict = RangeVerdict::Ignored;
            const std::optional<std::string> rangeHeader  = request.getHeader("range");
            if (rangeHeader.has_value() && isRangeApplicable(request, entityTagText, lastWriteSeconds))
            {
                rangeVerdict = parseByteRanges(*rangeHeader, fileSize, byteRanges);
                if (rangeVerdict == RangeVerdict::Satisfiable)
                {
                    byteRange = byteRanges.front();
                }
            }

            // 多个区间：拼一份 multipart/byteranges 交出去（RFC 9110 §14.6）。拼不成——正文里撞上了
            // 分隔符、文件在两次读之间换了版本、某段读失败——就把这条 Range 当没接，照常往下走回
            // 200 全量：一份自相矛盾的 206 比让对端重取整份文件糟得多
            if (rangeVerdict == RangeVerdict::MultiSatisfiable)
            {
                if (co_await writeMultipartByteRangesBody(candidatePath, byteRanges, fileSize, mimeType, *fileBasicInfo, isHeadRequest, response))
                {
                    response.setHeader("accept-ranges", "bytes");
                    appendCacheHeaders();
                    co_return;
                }
                byteRanges.clear();
                rangeVerdict = RangeVerdict::Ignored;
            }

            if (rangeVerdict == RangeVerdict::Unsatisfiable)
            {
                // 起点越界或后缀为 0：按 RFC 9110 §14.4 回 416，并如实报出当前表示的总长度
                response.setStatus(416);
                response.setHeader("content-range", "bytes */" + std::to_string(fileSize));
                response.setBody("Range Not Satisfiable");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            // 取这份文件的映射（仅 POSIX：那里 sendfile 是真零拷贝）：返回空指针时响应已被写成
            // 500/413，调用方直接收尾。先查缓存，命中即免去「打开文件 + 建立映射」那约 17 µs；
            // 未命中才真的建，建成再回填
#if !ASYN_PLATFORM_WIN32
            const auto prepareMappedFile = [&]() -> std::shared_ptr<const Platform::MemoryMappedFile>
            {
                if (const std::shared_ptr<const Platform::MemoryMappedFile> cached = settings->mappingCache->find(candidatePath, *fileBasicInfo); cached != nullptr)
                {
                    return cached;
                }

                auto mappedFile = std::make_shared<Platform::MemoryMappedFile>(Platform::MemoryMappedFile::open(candidatePath));
                if (!mappedFile->isValid())
                {
                    // 文件在 stat 之后被并发删除、改权限或占满句柄（TOCTOU 窗口）：按服务端故障处理，不回半个文件
                    response.setStatus(500);
                    response.setBody("Internal Server Error");
                    response.setHeader("content-type", "text/plain");
                    return nullptr;
                }
                // 映射长度才是正文的真实字节数。文件在 stat 与映射之间被换成更大的版本时，
                // 上面的上限判定已经过期，这里按新长度复查一次，避免绕过限制
                if (mappedFile->bytes().size() > kMaximumStaticFileSize)
                {
                    response.setStatus(413);
                    response.setBody("Payload Too Large");
                    response.setHeader("content-type", "text/plain");
                    return nullptr;
                }

                // 映射绑定的对象必须就是刚查到的那一份：stat 与 open 之间文件被原子替换时，
                // 发出去的是新版本的字节、配的是旧版本的验证器，客户端会把这份内容长期挂在旧 ETag 下。
                // 这里在登记之前判，对不上就不登记（登记了会让后续请求按旧元数据命中这份新映射）
                const std::optional<Platform::FileBasicInfo> mappedAs = mappedFile->openedFileInfo();
                if (!mappedAs.has_value() || mappedAs->sizeBytes != fileSize || mappedAs->lastWriteSeconds != lastWriteSeconds ||
                    mappedAs->identityTag != fileBasicInfo->identityTag)
                {
                    response.setStatus(500);
                    response.setBody("Internal Server Error");
                    response.setHeader("content-type", "text/plain");
                    return nullptr;
                }

                // 登记的元数据就是刚查到的那一份：下一次请求带着新的 size/mtime 来比，
                // 文件被换掉即不命中，不需要额外的失效通知通道
                settings->mappingCache->store(candidatePath, mappedFile, *fileBasicInfo);
                return mappedFile;
            };
#endif

            // 正文取哪段、取多少：206 只给区间那一段，200 给整份。HEAD 报的 content-length 也取自
            // 这两个数，与「同一个请求的 GET 会发多大」严格一致
            const std::size_t bodyOffset = rangeVerdict == RangeVerdict::Satisfiable ? static_cast<std::size_t>(byteRange.start) : 0U;
            const std::size_t bodyLength =
                    rangeVerdict == RangeVerdict::Satisfiable ? static_cast<std::size_t>(byteRange.end - byteRange.start + 1) : static_cast<std::size_t>(fileSize);

            // 表示头部一旦写下就与状态码绑定了，所以正文必须在此之前拿稳：这里失败的话，响应还只是
            // 一条光秃秃的错误（content-type + 正文），不会留下「500 带 Content-Range 与 ETag」这种
            // 自相矛盾的报文
#if ASYN_PLATFORM_WIN32
            // 本平台把正文读进响应自己的堆缓冲，不建映射。理由全是实测的：TransmitFile 在非阻塞
            // 套接字上仍会阻塞线程（不可用），所以映射并不能省掉「把字节交给内核」那次拷贝，只额外
            // 付出整段缺页 —— 同一文件两条路的中位数为 4 KiB 17.2–18.4/12.7–15.1 µs、
            // 64 KiB（含首触）29.4–31.1/15.5、1 MiB 343/46.7；且映射存活期间发布方的 rename 与就地
            // 截断分别以 5 与 1224 失败，读完即松手就没有这条占用。缓冲取自响应对象本身（按连接
            // 复用），第二条请求起这条路上一次堆分配都不发生
            if (!isHeadRequest)
            {
                std::string                                      &bodyBuffer = response.prepareBodyBuffer(bodyLength);
                Platform::FileBasicInfo                           openedBody;
                const std::expected<std::size_t, std::error_code> readResult = Platform::readFileContentsInto(candidatePath, bodyOffset, bodyLength, bodyBuffer, &openedBody);
                // 验证器取自查元数据那一次，正文取自这一次打开：两次之间文件被原子替换时，发出去的是
                // 新版本的字节配的却是旧版本的 ETag/Last-Modified，客户端会把这份内容长期挂在旧验证器
                // 下。两处必须是同一个对象，否则与短读一样按服务端故障收口
                // 正文长度为 0 时那一步压根没打开文件（少一次系统调用），openedBody 也就无从填起：
                // 空文件是一条合法表示，不能因为「比不了」被判成服务端故障
                const bool isVersionMismatch = bodyLength > 0U && (openedBody.sizeBytes != fileSize || openedBody.lastWriteSeconds != lastWriteSeconds ||
                                                                   openedBody.identityTag != fileBasicInfo->identityTag);
                if (!readResult.has_value() || *readResult < bodyLength || isVersionMismatch)
                {
                    // 两种情形都不是「可以发出去的正文」：文件在 stat 之后被删/改权限（TOCTOU 窗口），
                    // 或被截断到比请求的那段还短。按服务端故障收口，不回半个文件
                    response.setStatus(500);
                    response.setBody("Internal Server Error");
                    response.setHeader("content-type", "text/plain");
                    co_return;
                }
            }
#else
            std::shared_ptr<const Platform::MemoryMappedFile> mappedFile;
            if (!isHeadRequest)
            {
                mappedFile = prepareMappedFile();
                if (mappedFile == nullptr)
                {
                    co_return;
                }

                // stat 与映射之间文件被截断：请求区间已落在映射之外，按 500 收口，
                // 不让越界区间走到正文校验里变成异常
                if (rangeVerdict == RangeVerdict::Satisfiable && mappedFile->bytes().size() < static_cast<std::size_t>(byteRange.end) + 1)
                {
                    response.setStatus(500);
                    response.setBody("Internal Server Error");
                    response.setHeader("content-type", "text/plain");
                    co_return;
                }
            }
#endif

            // 200 与 206 共有的表示头部：都要声明支持按字节取区间
            response.setHeader("content-type", mimeType);
            response.setHeader("accept-ranges", "bytes");
            appendCacheHeaders();

            if (rangeVerdict == RangeVerdict::Satisfiable)
            {
                response.setStatus(206);
                response.setHeader("content-range", "bytes " + std::to_string(byteRange.start) + "-" + std::to_string(byteRange.end) + "/" + std::to_string(fileSize));

                // HEAD 只报「GET 会给出多大」：区间长度即 content-length，正文一个字节都不读
                if (isHeadRequest)
                {
                    response.setHeader("content-length", std::to_string(bodyLength));
                    co_return;
                }

#if !ASYN_PLATFORM_WIN32
                // 上限已限在 64 MiB 且区间经过 fileSize 钳制，折算到 size_t 不会窄化。
                // Windows 的这一段正文已经在上面就地写进响应缓冲，无需再挂任何东西
                response.setSharedMappedBody(mappedFile, bodyOffset, bodyLength);
#endif
                co_return;
            }

            response.setStatus(200);

            // HEAD 只报「GET 会给出多大」，正文一个字节都不必读：省下一次整文件 IO，
            // 同时把 content-length 显式写死，不会被响应序列化的「按正文重算」步骤抹平
            if (isHeadRequest)
            {
                response.setHeader("content-length", std::to_string(bodyLength));
                co_return;
            }

#if !ASYN_PLATFORM_WIN32
            // 映射整份文件当正文：不经过堆缓冲，发送时由 sendfile 直接把页交给内核，
            // 省掉「文件 → 堆正文」那次等量拷贝与分配。缓存让同一份页能同时服务多条在途响应。
            // 长度取映射自身而不是 stat 读数，因此正文与它自己描述的是同一份字节
            response.setSharedMappedBody(mappedFile, 0, mappedFile->bytes().size());
#endif
        }
    } // namespace

    HttpServer::HttpServer(Core::EventLoop &loop, const Core::InetAddress &address) :
        TcpServer(loop, address), m_limits(std::make_shared<const HttpServerLimits>()), m_metrics(std::make_shared<HttpMetricsCollector>()),
        m_requestIdGenerator(std::make_shared<HttpRequestIdGenerator>())
    {
        // 默认限额、统计与 request-id 生成器同样构造即就绪，理由见 metricsCollector() 的说明
        attachActiveConnectionMirror();
    }

    HttpServer::HttpServer(Core::EventLoop &loop, const int adoptedListeningDescriptor) :
        TcpServer(loop, adoptedListeningDescriptor), m_limits(std::make_shared<const HttpServerLimits>()), m_metrics(std::make_shared<HttpMetricsCollector>()),
        m_requestIdGenerator(std::make_shared<HttpRequestIdGenerator>())
    {
        // 与按地址构造的那一份同一接线：本服务器的连接数从一开始就并进自己的采集端
        attachActiveConnectionMirror();
    }

    void HttpServer::attachActiveConnectionMirror() noexcept
    {
        // 活跃连接数由连接管理器在增删连接的临界区内写进采集端：本台服务器只交出自己那一份，
        // 多台共用一份采集端时合起来的才是进程口径
        m_connectionManager.setSharedActiveCountMirror(&m_metrics->activeConnectionCountMirror());
    }

    Router &HttpServer::router()
    {
        return m_router;
    }

    std::shared_ptr<Core::Connection> HttpServer::createConnection(Core::AsyncSocket socket)
    {
        // 与基类契约的差异见头文件 Doxygen：这里只搬移 socket 与转交几个引用，
        // 不做握手、不查地址、不阻塞，因此既不抛异常也不可能返回空指针。
        // h2c 打开时明文连接进 HTTP/2 会话：它按先验知识直接进 HTTP/2 循环，不做协议嗅探
        if (m_isHttp2CleartextEnabled)
        {
            return std::make_shared<Http2Session>(m_loop, std::move(socket), m_router, m_limits, m_metrics, m_requestIdGenerator, m_parserLimits, m_memoryBudget,
                                                  m_http2Configuration);
        }
        return std::make_shared<HttpSession>(std::move(socket), m_router, m_limits, m_metrics, m_requestIdGenerator, m_parserLimits, m_memoryBudget);
    }

    void HttpServer::setHttp2Configuration(Http2ConnectionConfiguration configuration)
    {
        // 与连接层同一份判据：设置时就告状，别拖到第一条连接构造会话时才抛
        Http2Connection::validateConfiguration(configuration);
        m_http2Configuration = std::move(configuration);
    }

    const Http2ConnectionConfiguration &HttpServer::http2Configuration() const noexcept
    {
        return m_http2Configuration;
    }

    void HttpServer::setHttp2CleartextEnabled(const bool enabled) noexcept
    {
        m_isHttp2CleartextEnabled = enabled;
    }

    bool HttpServer::isHttp2CleartextEnabled() const noexcept
    {
        return m_isHttp2CleartextEnabled;
    }

    std::shared_ptr<HttpMetricsCollector> HttpServer::metricsCollector() const noexcept
    {
        return m_metrics;
    }

    void HttpServer::setMetricsCollector(std::shared_ptr<HttpMetricsCollector> collector)
    {
        // 空采集端会让本服务器彻底没有计数出口：/metrics 与 stats() 一起变成零，
        // 而这看起来与「没有流量」一模一样，事后极难发现，因此按用法错误直接拒绝
        if (collector == nullptr)
        {
            throw Base::InvalidArgumentException("HttpServer: 统计采集端不能为空，请传入一份现成的采集端或调用 metricsCollector() 取本服务器的");
        }

        // 旧采集端要撑到镜像重接完为止：重接时连接管理器会把本台在册的连接数从**旧**那一份里退掉，
        // 而 `m_metrics = ...` 一旦交回最后一份引用，旧对象连同它内嵌的那个原子量就当场没了——
        // 那一次退账就写在已释放的内存上（ConnectionManager 的契约：镜像对象的生存期要覆盖本管理器）
        const std::shared_ptr<HttpMetricsCollector> previous = m_metrics;
        m_metrics                                            = std::move(collector);
        // 换采集端要连同镜像一起重接：否则新采集端上的活跃连接数永远停在旧目标上
        attachActiveConnectionMirror();
    }

    std::shared_ptr<HttpRequestIdGenerator> HttpServer::requestIdGenerator() const noexcept
    {
        return m_requestIdGenerator;
    }

    HttpServerStats HttpServer::stats() const
    {
        // 活跃连接数已经在这份快照里：它是连接管理器在增删连接的临界区内写进采集端的镜像，
        // 与在册表同时刻变化，不需要在这里再读一次本实例的连接表（那样只能报出本实例那一份）
        HttpServerStats snapshot = m_metrics->snapshot();
        // 三个数一起并入：只报分子（在册数与拒过的条数）而不报分母，抓取端算不出「离上限还有多远」，
        // 只能靠人记得配置文件里写过什么——而多进程时配置是整机数、生效的是摊到本进程那一份
        applyAdmissionSnapshot(snapshot, maximumConnections(), perIpConnectionLimiter() == nullptr ? std::size_t{0} : perIpConnectionLimiter()->maximumConnectionsPerIp(),
                               perIpRejectedConnectionCount());
        // 本身上限的满载是另一件事：它说明整机容量到顶，而不是某个来源在刷
        snapshot.overLimitRejectedConnectionCount = overLimitRejectedConnectionCount();
        return snapshot;
    }

    void HttpServer::enableMetricsEndpoint(const std::string_view path, const std::string_view metricNamePrefix)
    {
        // 路径形状先拦下来：不以 / 开头的路径永远匹配不到任何请求，静默注册等于给人一个
        // 「明明调了却没有端点」的假象，而调用方很难自己看出来
        if (path.empty() || path.front() != '/')
        {
            throw Base::InvalidArgumentException("HttpServer: 指标端点路径必须以 / 开头，收到的是「" + std::string(path) + "」");
        }

        // 前缀也要过 Prometheus 的名字语法（与登记侧同一份判据）：带 `.`、`-` 或空格的前缀会安静地
        // 拼出 `asyn.srv_requests_total` 这类整片非法名字——端点照样回 200，而抓取端把每一行都拒掉，
        // 现场看到的是「有 /metrics 却什么都抓不到」。登记侧对同一个名字是当场拒的，两条路不能两种待遇
        if (!metricNamePrefix.empty() && !Core::isLegalPrometheusMetricName(metricNamePrefix))
        {
            throw Base::InvalidArgumentException("HttpServer: 指标名前缀「" + std::string(metricNamePrefix) +
                                                 "」不合 Prometheus 的名字语法（[a-zA-Z_:][a-zA-Z0-9_:]*）；"
                                                 "请只用字母数字与下划线，或留空表示不加前缀");
        }

        // 前缀按值捕进处理函数：字符串是调用方的，可能比服务器先走；这里只留一份拷贝
        const std::string metricPrefix(metricNamePrefix);
        m_router.get(std::string(path),
                     [this, metricPrefix](HttpRequest &, HttpResponse &response) -> Core::Task<>
                     {
                         // 每次抓取现取一次快照：计数是原子的，不必把动作投递到事件循环
                         response.setStatus(200);
                         response.setHeader("content-type", kPrometheusTextContentType);
                         response.setBody(formatPrometheusMetrics(stats(), metricPrefix));
                         co_return;
                     });
    }

    void HttpServer::enableHealthEndpoint(const std::string_view path)
    {
        // 与指标端点同样的形状校验，理由同上
        if (path.empty() || path.front() != '/')
        {
            throw Base::InvalidArgumentException("HttpServer: 健康检查端点路径必须以 / 开头，收到的是「" + std::string(path) + "」");
        }

        m_router.get(std::string(path),
                     [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                     {
                         // 应答固定且无依赖：能走到这里就说明事件循环在转、连接还能被服务（存活性）
                         response.setStatus(200);
                         response.setHeader("content-type", "application/json");
                         response.setBody(kHealthCheckResponseBody);
                         co_return;
                     });
    }

    void HttpServer::enableReadinessEndpoint(const std::string_view path)
    {
        // 与另两个端点同样的形状校验：不以 / 开头的路径永远匹配不到，静默注册就是给人一个假象
        if (path.empty() || path.front() != '/')
        {
            throw Base::InvalidArgumentException("HttpServer: 就绪探针端点路径必须以 / 开头，收到的是「" + std::string(path) + "」");
        }

        // 判据现读 isRunning()：排空没有推送口，编排器只能靠轮询问出来，缓存一份就会把「已经停了」
        // 晚一拍报出去
        registerReadinessEndpoint(m_router, path, [this] { return isRunning(); });
    }

    void HttpServer::enableLoopDiagnosticsEndpoint(const std::string_view path)
    {
        // 与另两个端点同样的形状校验：不以 / 开头的路径永远匹配不到，静默注册就是给人一个假象
        if (path.empty() || path.front() != '/')
        {
            throw Base::InvalidArgumentException("HttpServer: 事件循环观测端点路径必须以 / 开头，收到的是「" + std::string(path) + "」");
        }

        m_router.get(std::string(path),
                     [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                     {
                         // 现取整表现场渲染：读的都是各条循环自己的原子量，不需要把动作投进任何一条循环
                         response.setStatus(200);
                         response.setHeader("content-type", kLoopDiagnosticsContentType);
                         response.setBody(formatLoopDiagnosticsJson(Core::eventLoopSnapshots(), Core::unregisteredEventLoopCount(), std::chrono::steady_clock::now()));
                         co_return;
                     });
    }

    void StaticFileService::install(Router &router, const std::size_t maximumMappedStaticFiles)
    {
        // 配置本体与兜底路由一起建立：注册一次之后处理函数只读配置，于是「改目录」「关静态服务」
        // 「改 Cache-Control」都只写这一份，不会出现第二条 "*" 路由或新旧目录同时在服务
        if (m_settings != nullptr)
        {
            return;
        }

        m_settings = std::make_shared<StaticFileSettings>();
        // 缓存随配置一起建立：上限取登记那一刻的限额（限额要在静态目录之前设），之后只做读写、
        // 不再重建，处理函数因此只依赖 settings 而不依赖任何一台服务器本身
        std::size_t mappedFileCountLimit = maximumMappedStaticFiles;
#if ASYN_PLATFORM_WIN32
        // Windows 上刻意不缓存映射：文件只要还挂着一个活动映射，既不能就地截断，也不能被 rename
        // 覆盖（实测 5 与 1224，共享位换不来这两条）。「写临时文件 + rename」是静态资源发布的常规
        // 做法，让缓存把它挡掉，代价比省下的那次「打开 + 建映射」重得多。本平台的静态正文因此走
        // 堆读取、压根不建映射（实测更省，见 serveStaticFileRequest 的说明），这个限额在 Windows 上
        // 无论取什么值都不再有作用；POSIX 不受影响。
        if (mappedFileCountLimit != 0)
        {
            LOG_INFO("StaticFile: 本平台不启用静态文件映射缓存（映射期间文件无法被替换或截断），"
                     "已按关闭处理；需要省掉这次映射开销请把服务跑在 POSIX 平台上");
            mappedFileCountLimit = 0;
        }
#endif
        m_settings->mappingCache = std::make_shared<StaticFileMappingCache>(mappedFileCountLimit);
        router.any("*", [settings = m_settings](HttpRequest &request, HttpResponse &response) -> Core::Task<> { co_await serveStaticFileRequest(request, response, settings); });
    }

    void StaticFileService::setDirectory(const std::string &directoryPath)
    {
        // 空串按「关闭静态服务」处理：比让调用方传一个不存在的目录再等它规范化失败更直白
        if (directoryPath.empty())
        {
            m_settings->isEnabled = false;
            m_settings->rootDirectory.clear();
            return;
        }

        std::error_code canonicalError;
        // 配置里的目录文本按 UTF-8 解释：直接交给 path 会让 Windows 拿本地代码页读这段字节，中文站点
        // 目录会被规范化成另一个名字，之后每个请求都在那个名字下找不到的文件上
        const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(Platform::FileSystem::pathFromUtf8(directoryPath), canonicalError);

        // 规范化失败（无权限、路径过长）就关掉静态服务并记中文告警：
        // 把问题留在配置时刻，好过在每个请求上都复现一次不确定行为
        if (canonicalError)
        {
            m_settings->isEnabled = false;
            m_settings->rootDirectory.clear();
            LOG_WARN_FMT("StaticFile: 静态目录规范化失败，已关闭静态文件服务。目录：{}，原因：{}", directoryPath, canonicalError.message());
            return;
        }

        // 「目录此刻不存在」单独判一次：weakly_canonical 对不存在的路径是按词法规范化的，它会安静地
        // 成功，只看它的错误码就等于把「配置的目录根本没建」这种最常见的拼错放过——之后每个请求都
        // 回 404，而配置面读起来一切正常。文档承诺的是配置时刻就报出来，这里补齐那道判定
        std::error_code existsError;
        if (!std::filesystem::is_directory(canonicalRoot, existsError))
        {
            m_settings->isEnabled = false;
            m_settings->rootDirectory.clear();
            LOG_WARN_FMT("StaticFile: 静态目录不存在或不是目录，已关闭静态文件服务。目录：{}（规范化为 {}），原因：{}", directoryPath,
                         Platform::FileSystem::utf8FromPath(canonicalRoot), existsError ? existsError.message() : "该路径不是一个已存在的目录");
            return;
        }

        // 配置在此落定：之后每个请求都拿这份绝对路径去做包含判定，不再碰文件系统去解析根目录本身
        m_settings->rootDirectory = canonicalRoot;
        m_settings->isEnabled     = true;
    }

    std::string StaticFileService::directory() const
    {
        if (m_settings == nullptr || !m_settings->isEnabled)
        {
            return {};
        }
        // 出去的口与进来的口同一刻度：调用方按 UTF-8 配的目录，读回来也必须是 UTF-8 文本
        // （Windows 上 path::string() 给的是本地代码页的字节，中文目录名会读回另一串）
        return Platform::FileSystem::utf8FromPath(m_settings->rootDirectory);
    }

    void StaticFileService::setCacheControl(const std::optional<std::string> cacheControl)
    {
        // 值会被原样写进头部块：含 CR/LF/NUL 就等于让调用方提前结束头部块（响应拆分），
        // 与静态目录配置一致，问题留在配置时刻暴露并记中文告警，而不是每请求静默少一条头
        if (cacheControl.has_value() &&
            (cacheControl->find('\r') != std::string::npos || cacheControl->find('\n') != std::string::npos || cacheControl->find('\0') != std::string::npos))
        {
            m_settings->cacheControl.reset();
            LOG_WARN_FMT("StaticFile: 静态文件 Cache-Control 含非法字符（CR/LF/NUL），已忽略该配置。值：{}", *cacheControl);
            return;
        }

        m_settings->cacheControl = cacheControl;
    }

    void StaticFileService::setDirectoryListing(const bool enabled)
    {
        m_settings->listingEnabled = enabled;
    }

    std::shared_ptr<StaticFileSettings> StaticFileService::settings() const noexcept
    {
        return m_settings;
    }

    void HttpServer::ensureStaticFileSettings()
    {
        m_staticFiles.install(m_router, m_limits->maximumMappedStaticFiles);
    }

    void HttpServer::staticFileDir(const std::string &directoryPath)
    {
        ensureStaticFileSettings();
        m_staticFiles.setDirectory(directoryPath);
    }

    std::string HttpServer::staticFileDir() const
    {
        return m_staticFiles.directory();
    }

    void HttpServer::staticDirectoryListing(const bool enabled)
    {
        // 与另外两条静态配置同一条路：本体在 StaticFileService，服务器一侧只转发，
        // 明文/TLS/h3 三条通道因此不会在「目录列不列」上给出不同答案
        ensureStaticFileSettings();
        m_staticFiles.setDirectoryListing(enabled);
    }

    void HttpServer::setStaticFileCacheControl(const std::optional<std::string> cacheControl)
    {
        ensureStaticFileSettings();
        m_staticFiles.setCacheControl(cacheControl);
    }

    std::optional<std::string> HttpServer::staticFileCacheControl() const
    {
        const std::shared_ptr<StaticFileSettings> settings = m_staticFiles.settings();
        return settings == nullptr ? std::nullopt : settings->cacheControl;
    }

    void HttpServer::setLimits(HttpServerLimits limits)
    {
        // 换一份新配置而不是改写原对象：会话按 shared_ptr 只读持有它，
        // 就地修改会让在途会话读到半新半旧的组合
        m_limits = std::make_shared<const HttpServerLimits>(limits);
    }

    HttpServerLimits HttpServer::limits() const
    {
        return *m_limits;
    }

    void HttpServer::setMemoryBudget(std::shared_ptr<HttpMemoryBudget> memoryBudget)
    {
        // 共享指针按值存：预算跨连接、跨监听器共用一份账，持有者什么时候撒手都不影响在途会话
        m_memoryBudget = std::move(memoryBudget);
    }

    const std::shared_ptr<HttpMemoryBudget> &HttpServer::memoryBudget() const noexcept
    {
        return m_memoryBudget;
    }

    void HttpServer::setParserLimits(HttpParserLimits limits)
    {
        // 按值保存而不是共享指针：解析上限只在会话构造那一刻被解析器取走一份副本，
        // 之后没有读者，因此不需要「整体换代」那套共享只读机制
        m_parserLimits = limits;
    }

    HttpParserLimits HttpServer::parserLimits() const
    {
        return m_parserLimits;
    }

} // namespace AsynGyanis::Net
