#include "Net/Http/HttpServer.h"

#include "Net/Http/HttpMemoryBudget.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Task.h"
#include "Net/Http/FileSender.h"
#include "Net/Http/HttpDate.h"
#include "Net/Http/HttpMetricsEndpoint.h"
#include "Net/Http/HttpSession.h"
#include "Net/Http2/Http2Session.h"
#include "Platform/IO/MemoryMappedFile.h"

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
         *          因此必须设上限。需要服务更大文件时应先补跨平台 sendfile 封装（Platform 层）。
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
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f') ||
                   (character >= 'A' && character <= 'F');
        }

        /**
         * @brief 取十六进制字符的数值
         * @param character 已确认为十六进制字符的输入
         * @return int 0~15
         */
        constexpr int hexadecimalDigitValue(const char character)
        {
            if (character >= '0' && character <= '9')
            {
                return character - '0';
            }
            return (character >= 'a' ? character - 'a' : character - 'A') + 10;
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
            std::string_view remainder = segmentsView;

            while (!remainder.empty())
            {
                const std::size_t slashPosition = remainder.find('/');
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
         * @brief 去掉首尾 OWS（空格与水平制表符）
         * @param text 原始文本
         * @return 去掉首尾空白后的视图
         */
        std::string_view trimOptionalWhitespace(std::string_view text)
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
            {
                text.remove_suffix(1);
            }
            return text;
        }

        /**
         * @brief 判断两段 ASCII 文本是否相等（忽略大小写）
         * @param left 左操作数
         * @param right 右操作数
         * @return true 长度相同且逐字符相等（a~z 与 A~Z 视为同一字符）
         */
        bool equalsIgnoreCase(const std::string_view left, const std::string_view right)
        {
            if (left.size() != right.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < left.size(); ++index)
            {
                const auto leftCharacter = static_cast<unsigned char>(left[index]);
                const auto rightCharacter = static_cast<unsigned char>(right[index]);
                if (std::tolower(leftCharacter) != std::tolower(rightCharacter))
                {
                    return false;
                }
            }
            return true;
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
            std::uintmax_t parsedValue = 0;
            const auto [endPointer, errorCode] = std::from_chars(text.data(), text.data() + text.size(), parsedValue);
            if (errorCode != std::errc() || endPointer != text.data() + text.size())
            {
                return false;
            }
            value = parsedValue;
            return true;
        }

        /**
         * @brief 把文件系统时间折算成 system_clock 时间点
         * @details file_time_type 的纪元由实现决定（MSVC 为 1601-01-01，libstdc++ 为 1970-01-01），
         *          与 system_clock 不同源，不能直接当秒数相加；用「同一瞬间两个 now() 的差值」
         *          折算，可移植地绕开 std::chrono::clock_cast 的可用性问题。
         * @param fileTime 文件系统时间
         * @return 对应的 system_clock 时间点
         */
        std::chrono::system_clock::time_point toSystemClockTime(const std::filesystem::file_time_type fileTime)
        {
            const std::filesystem::file_time_type fileNow = std::filesystem::file_time_type::clock::now();
            const std::chrono::system_clock::time_point systemNow = std::chrono::system_clock::now();
            return std::chrono::time_point_cast<std::chrono::system_clock::duration>(systemNow + (fileTime - fileNow));
        }

        /**
         * @brief 按「文件大小 + 修改时间整秒」构造强 ETag
         * @param fileSize 文件字节数
         * @param lastWriteSeconds 文件修改时间，自 Unix 纪元起的秒数
         * @return 形如 "\"1a-5f2c3d4e\"" 的带双引号标签；大小或修改时间任一变化都会改变它
         */
        std::string makeStrongEtag(const std::uintmax_t fileSize, const std::int64_t lastWriteSeconds)
        {
            // 缓冲按 64 位上限给足（无符号十六进制最多 16 位，有符号再含负号），to_chars 不会写不下
            std::array<char, 24> sizeText{};
            const auto sizeResult = std::to_chars(sizeText.data(), sizeText.data() + sizeText.size(), fileSize, 16);

            std::array<char, 24> timeText{};
            const auto timeResult = std::to_chars(timeText.data(), timeText.data() + timeText.size(), lastWriteSeconds, 16);

            std::string etagText;
            etagText.reserve(2 + sizeText.size() + 1 + timeText.size());
            etagText.push_back('"');
            etagText.append(sizeText.data(), static_cast<std::size_t>(sizeResult.ptr - sizeText.data()));
            etagText.push_back('-');
            etagText.append(timeText.data(), static_cast<std::size_t>(timeResult.ptr - timeText.data()));
            etagText.push_back('"');
            return etagText;
        }

        /**
         * @brief 去掉弱 ETag 的 "W/" 前缀
         * @details If-None-Match 按弱比较：W/"x" 与 "x" 视为同一标签（RFC 9110 §13.1.2）。
         * @param entityTag 待处理的标签
         * @return 去掉前缀后的标签；本来没有前缀时原样返回
         */
        std::string_view stripWeakPrefix(std::string_view entityTag)
        {
            // "W/" 必须是大写 W，这是 RFC 9110 §8.8.3 的固定写法
            if (entityTag.starts_with("W/"))
            {
                entityTag.remove_prefix(2);
            }
            return entityTag;
        }

        /**
         * @brief 判断 If-None-Match 的值是否命中本资源的 ETag
         * @details 值为 "*" 或列表里任一标签（弱比较）与本资源标签相同即命中。
         * @param listValue If-None-Match 头原文
         * @param entityTag 本资源当前 ETag
         * @return true 命中
         */
        bool entityTagListMatches(const std::string_view listValue, const std::string_view entityTag)
        {
            std::string_view remainder = listValue;
            while (true)
            {
                const std::size_t commaPosition = remainder.find(',');
                const std::string_view candidate = trimOptionalWhitespace(remainder.substr(0, commaPosition));
                if (candidate == "*" || stripWeakPrefix(candidate) == entityTag)
                {
                    return true;
                }
                if (commaPosition == std::string_view::npos)
                {
                    break;
                }
                remainder = remainder.substr(commaPosition + 1);
            }
            return false;
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
         *          或解析为日期且不早于 Last-Modified（RFC 9110 §13.1.5）。
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
            return lastWriteSeconds <= ifRangeSeconds;
        }

        /// 单个字节区间的解析结论
        enum class RangeVerdict
        {
            Ignored,       ///< 不存在、语法非法或含多个区间：忽略该头，按 200 全量返回
            Unsatisfiable, ///< 起点超出表示长度或后缀为 0：应答 416
            Satisfiable    ///< 得到一个可用的闭区间 [start, end]
        };

        /**
         * @brief 单个字节区间的解析结果
         */
        struct ByteRange
        {
            std::uintmax_t start{0}; ///< 区间起始偏移（含）
            std::uintmax_t end{0};   ///< 区间结束偏移（含）
        };

        /**
         * @brief 解析 Range 头里的单个字节区间
         * @details 只支持 bytes 单位的单个区间：单位不符、语法非法或含多个区间时按 RFC 允许的
         *          做法整条忽略（回 200 全量）；`bytes=start-end`、`bytes=start-`、`bytes=-suffix`
         *          三种形态都支持，end 超出表示末尾时截到末尾。
         * @param headerValue Range 头原文
         * @param representationSize 当前表示的字节数
         * @param byteRange 输出：可满足的闭区间（仅返回 Satisfiable 时有效）
         * @return RangeVerdict 解析结论
         */
        RangeVerdict parseSingleByteRange(const std::string_view headerValue, const std::uintmax_t representationSize, ByteRange &byteRange)
        {
            byteRange = ByteRange{};

            const std::size_t equalsPosition = headerValue.find('=');
            if (equalsPosition == std::string_view::npos)
            {
                return RangeVerdict::Ignored;
            }
            // 区间单位大小写不敏感（RFC 9110 §14.1）
            if (!equalsIgnoreCase(trimOptionalWhitespace(headerValue.substr(0, equalsPosition)), "bytes"))
            {
                return RangeVerdict::Ignored;
            }

            const std::string_view rangeSpec = trimOptionalWhitespace(headerValue.substr(equalsPosition + 1));
            if (rangeSpec.empty() || rangeSpec.find(',') != std::string_view::npos)
            {
                // 多区间请求整条忽略：本服务器不做 multipart/byteranges 拼装
                return RangeVerdict::Ignored;
            }
            if (representationSize == 0)
            {
                // 空表示不存在任何可满足的区间
                return RangeVerdict::Unsatisfiable;
            }

            if (rangeSpec.front() == '-')
            {
                // 后缀形态：取末尾 suffixLength 个字节
                std::uintmax_t suffixLength = 0;
                if (!parseUnsignedDecimal(rangeSpec.substr(1), suffixLength))
                {
                    return RangeVerdict::Ignored;
                }
                if (suffixLength == 0)
                {
                    return RangeVerdict::Unsatisfiable;
                }
                byteRange.start = (suffixLength >= representationSize) ? 0 : representationSize - suffixLength;
                byteRange.end = representationSize - 1;
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
                // 起点已在表示之外：不可满足
                return RangeVerdict::Unsatisfiable;
            }

            const std::string_view endText = rangeSpec.substr(dashPosition + 1);
            std::uintmax_t endValue = representationSize - 1;
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
            byteRange.end = endValue;
            return RangeVerdict::Satisfiable;
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

            // 请求打到目录本身：本服务器不做目录索引，也不返回目录内容，按 404 处理
            if (relativeText.empty())
            {
                response.setStatus(404);
                response.setBody("Not Found");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            std::error_code pathError;
            const std::filesystem::path candidatePath = std::filesystem::weakly_canonical(settings->rootDirectory / relativeText, pathError);
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

            std::error_code statusError;
            if (!std::filesystem::is_regular_file(candidatePath, statusError) || statusError)
            {
                // 不存在、是目录、是设备文件，或 stat 失败：一律 404，避免把目录结构泄露给探测者
                response.setStatus(404);
                response.setBody("Not Found");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            const std::uintmax_t fileSize = std::filesystem::file_size(candidatePath, statusError);
            if (statusError)
            {
                response.setStatus(500);
                response.setBody("Internal Server Error");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            // 内存保护：超限的大文件不映射，也不给半截正文
            if (fileSize > kMaximumStaticFileSize)
            {
                response.setStatus(413);
                response.setBody("Payload Too Large");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            std::error_code metadataError;
            const std::filesystem::file_time_type lastWriteFileTime = std::filesystem::last_write_time(candidatePath, metadataError);
            if (metadataError)
            {
                // 取不到修改时间就给不出可用的验证器，按服务端故障收口，而不是发一条没有 Last-Modified 的响应
                response.setStatus(500);
                response.setBody("Internal Server Error");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            // 验证器：Last-Modified 由文件系统时间折算到 system_clock，ETag 由「大小 + 修改时间整秒」构出
            const std::chrono::system_clock::time_point lastWriteTimePoint = toSystemClockTime(lastWriteFileTime);
            const std::int64_t lastWriteSeconds = std::chrono::duration_cast<std::chrono::seconds>(lastWriteTimePoint.time_since_epoch()).count();
            const std::string entityTagText = makeStrongEtag(fileSize, lastWriteSeconds);
            const std::string lastModifiedText = formatHttpDate(std::chrono::system_clock::time_point(std::chrono::seconds(lastWriteSeconds)));
            const std::string mimeType = FileSender::contentTypeForFile(candidatePath.string());

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

            // 条件请求：命中验证器即 304，无正文，也不必打开文件映射
            if (isNotModified(request, entityTagText, lastWriteSeconds))
            {
                response.setStatus(304);
                appendCacheHeaders();
                co_return;
            }

            // Range：仅在 If-Range 放行时解析，不放行时按 200 全量
            ByteRange byteRange{};
            RangeVerdict rangeVerdict = RangeVerdict::Ignored;
            const std::optional<std::string> rangeHeader = request.getHeader("range");
            if (rangeHeader.has_value() && isRangeApplicable(request, entityTagText, lastWriteSeconds))
            {
                rangeVerdict = parseSingleByteRange(*rangeHeader, fileSize, byteRange);
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

            // 打开映射并复核长度：返回 false 时响应已被写成 500/413，调用方直接收尾
            const auto prepareMappedFile = [&candidatePath, &response](Platform::MemoryMappedFile &mappedFile) -> bool
            {
                mappedFile = Platform::MemoryMappedFile::open(candidatePath);
                if (!mappedFile.isValid())
                {
                    // 文件在 stat 之后被并发删除、改权限或占满句柄（TOCTOU 窗口）：按服务端故障处理，不回半个文件
                    response.setStatus(500);
                    response.setBody("Internal Server Error");
                    response.setHeader("content-type", "text/plain");
                    return false;
                }
                // 映射长度才是正文的真实字节数。文件在 stat 与映射之间被换成更大的版本时，
                // 上面的上限判定已经过期，这里按新长度复查一次，避免绕过限制
                if (mappedFile.bytes().size() > kMaximumStaticFileSize)
                {
                    response.setStatus(413);
                    response.setBody("Payload Too Large");
                    response.setHeader("content-type", "text/plain");
                    return false;
                }
                return true;
            };

            // 200 与 206 共有的表示头部：都要声明支持按字节取区间
            response.setHeader("content-type", mimeType);
            response.setHeader("accept-ranges", "bytes");
            appendCacheHeaders();

            if (rangeVerdict == RangeVerdict::Satisfiable)
            {
                const std::uintmax_t rangeLength = byteRange.end - byteRange.start + 1;
                response.setStatus(206);
                response.setHeader("content-range",
                                   "bytes " + std::to_string(byteRange.start) + "-" + std::to_string(byteRange.end) + "/" + std::to_string(fileSize));

                // HEAD 只报「GET 会给出多大」：区间长度即 content-length，正文一个字节都不读
                if (isHeadRequest)
                {
                    response.setHeader("content-length", std::to_string(rangeLength));
                    co_return;
                }

                Platform::MemoryMappedFile mappedFile;
                if (!prepareMappedFile(mappedFile))
                {
                    co_return;
                }
                // stat 与映射之间文件被截断：请求区间已落在映射之外，按 500 收口，
                // 不让越界区间走到区间校验里变成异常
                if (mappedFile.bytes().size() < static_cast<std::size_t>(byteRange.end) + 1)
                {
                    response.setStatus(500);
                    response.setBody("Internal Server Error");
                    response.setHeader("content-type", "text/plain");
                    co_return;
                }

                // 上限已限在 64 MiB 且区间经过 fileSize 钳制，折算到 size_t 不会窄化
                response.setMappedBody(std::move(mappedFile), static_cast<std::size_t>(byteRange.start), static_cast<std::size_t>(rangeLength));
                co_return;
            }

            response.setStatus(200);

            // HEAD 只报「GET 会给出多大」，正文一个字节都不必读：省下一次整文件 IO，
            // 同时把 content-length 显式写死，不会被响应序列化的「按正文重算」步骤抹平
            if (isHeadRequest)
            {
                response.setHeader("content-length", std::to_string(static_cast<std::uint64_t>(fileSize)));
                co_return;
            }

            // 映射整份文件当正文：不经过堆缓冲，发送时由聚合写直接引用文件页，
            // 省掉「文件 → 堆正文」那次等量拷贝与分配。映射对象随响应存活，发送期间一定有效
            Platform::MemoryMappedFile mappedFile;
            if (!prepareMappedFile(mappedFile))
            {
                co_return;
            }

            response.setMappedBody(std::move(mappedFile));
        }
    } // namespace

    HttpServer::HttpServer(Core::EventLoop &loop, const Core::InetAddress &address) :
        TcpServer(loop, address),
        m_limits(std::make_shared<const HttpServerLimits>()),
        m_metrics(std::make_shared<HttpMetricsCollector>()),
        m_requestIdGenerator(std::make_shared<HttpRequestIdGenerator>())
    {
        // 构造即给出一份默认限额：会话永远拿得到非空配置，不必在创建路径上判空。
        // 统计与 request-id 生成器同样构造即就绪：它们没有开关，采集是常开行为，
        // 会话按 shared_ptr 共享持有，因此生命周期一定覆盖所有会话
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
            return std::make_shared<Http2Session>(std::move(socket), m_router, m_limits, m_metrics, m_requestIdGenerator, m_parserLimits,
                                                 m_memoryBudget);
        }
        return std::make_shared<HttpSession>(std::move(socket), m_router, m_limits, m_metrics, m_requestIdGenerator, m_parserLimits,
                                             m_memoryBudget);
    }

    void HttpServer::setHttp2CleartextEnabled(const bool enabled) noexcept
    {
        m_isHttp2CleartextEnabled = enabled;
    }

    bool HttpServer::isHttp2CleartextEnabled() const noexcept
    {
        return m_isHttp2CleartextEnabled;
    }

    HttpServerStats HttpServer::stats() const
    {
        HttpServerStats snapshot = m_metrics->snapshot();

        // 活跃连接数只有一个真值来源（连接管理器）：另设一份计数迟早与它漂移，
        // 因此每次取快照都现读一次。size_t 到 uint64_t 是加宽转换，32 位平台上也不会丢信息
        snapshot.activeConnectionCount = static_cast<std::uint64_t>(m_connectionManager.activeCount());
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

        // 前缀按值捕进处理函数：字符串是调用方的，可能比服务器先走；这里只留一份拷贝
        const std::string metricPrefix(metricNamePrefix);
        m_router.get(std::string(path),
                     [this, metricPrefix](HttpRequest &, HttpResponse &response) -> Core::Task<>
                     {
                         // 每次抓取现取一次快照：计数是原子的，不必把动作投递到事件循环
                         response.setStatus(200);
                         response.setHeader("content-type", std::string(kPrometheusTextContentType));
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

    void HttpServer::ensureStaticFileSettings()
    {
        // 设置对象与兜底路由一起建立：注册一次之后处理函数只读配置，
        // 于是「改目录」「关静态服务」「改 Cache-Control」都只写配置，不再有第二条 "*" 路由
        if (m_staticFileSettings != nullptr)
        {
            return;
        }

        m_staticFileSettings = std::make_shared<StaticFileSettings>();
        m_router.any("*", [settings = m_staticFileSettings](HttpRequest &request, HttpResponse &response) -> Core::Task<>
        {
            co_await serveStaticFileRequest(request, response, settings);
        });
    }

    void HttpServer::staticFileDir(const std::string &directoryPath)
    {
        ensureStaticFileSettings();

        // 空串按「关闭静态服务」处理：比让调用方传一个不存在的目录再等它规范化失败更直白
        if (directoryPath.empty())
        {
            m_staticFileSettings->isEnabled = false;
            m_staticFileSettings->rootDirectory.clear();
            return;
        }

        std::error_code canonicalError;
        const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(std::filesystem::path(directoryPath), canonicalError);

        // 规范化失败（含目录不存在、无权限、路径过长）就关掉静态服务并记中文告警：
        // 把问题留在配置时刻，好过在每个请求上都复现一次不确定行为
        if (canonicalError)
        {
            m_staticFileSettings->isEnabled = false;
            m_staticFileSettings->rootDirectory.clear();
            LOG_WARN_FMT("HttpServer: 静态目录规范化失败，已关闭静态文件服务。目录：{}，原因：{}", directoryPath, canonicalError.message());
            return;
        }

        // 配置在此落定：之后每个请求都拿这份绝对路径去做包含判定，不再碰文件系统去解析根目录本身
        m_staticFileSettings->rootDirectory = canonicalRoot;
        m_staticFileSettings->isEnabled     = true;
    }

    std::string HttpServer::staticFileDir() const
    {
        if (m_staticFileSettings == nullptr || !m_staticFileSettings->isEnabled)
        {
            return {};
        }
        return m_staticFileSettings->rootDirectory.string();
    }

    void HttpServer::setStaticFileCacheControl(const std::optional<std::string> cacheControl)
    {
        ensureStaticFileSettings();

        // 值会被原样写进头部块：含 CR/LF/NUL 就等于让调用方提前结束头部块（响应拆分），
        // 与静态目录配置一致，问题留在配置时刻暴露并记中文告警，而不是每请求静默少一条头
        if (cacheControl.has_value() &&
            (cacheControl->find('\r') != std::string::npos || cacheControl->find('\n') != std::string::npos || cacheControl->find('\0') != std::string::npos))
        {
            m_staticFileSettings->cacheControl.reset();
            LOG_WARN_FMT("HttpServer: 静态文件 Cache-Control 含非法字符（CR/LF/NUL），已忽略该配置。值：{}", *cacheControl);
            return;
        }

        m_staticFileSettings->cacheControl = cacheControl;
    }

    std::optional<std::string> HttpServer::staticFileCacheControl() const
    {
        if (m_staticFileSettings == nullptr)
        {
            return std::nullopt;
        }
        return m_staticFileSettings->cacheControl;
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
