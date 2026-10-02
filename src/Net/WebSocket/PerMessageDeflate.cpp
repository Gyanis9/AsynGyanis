#include "Net/WebSocket/PerMessageDeflate.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 每条压缩消息末尾的固定四字节空块尾（RFC 7692 §7.2.1）：线上负载不含它，收发两侧各自增删
        constexpr std::array<char, 4> kDeflateTail{'\x00', '\x00', '\xFF', '\xFF'};

        /// 逐块解压时的块大小
        constexpr std::size_t kInflateChunkBytes = 16 * 1024;

        /// 压缩输出缓冲相对输入的余量：同步刷新的空块最多几字节，给 64 足够且不占什么内存
        constexpr std::size_t kDeflateOutputHeadroomBytes = 64;

        /**
         * @brief 本线程复用的压缩流：避免每条消息重建一次 deflate 状态
         *
         * @details 协商里两端都选了 `*_no_context_takeover`（每条消息不延续字典），这恰好等于
         *          `deflateReset` 的语义——把流恢复到「刚 init 完」的状态，产出与「每条消息新建一条流」
         *          逐字节一致，却免去每次 deflateInit2/End 重建约 200KB 内部状态（短消息上那比压缩本身还贵）。
         *          thread_local 让每条事件循环线程各持一份，天然无跨线程共享；出错路径就地 End 并标记关闭，
         *          下一次调用重新 init，绝不在可疑状态上继续复用。
         *          窗口位数是**按连接**协商出来的，而同一条线程会服务多条连接，所以复用的前提加上
         *          「位数与上次 init 的一致」：不一致就先 End 再按新位数 init，绝不能沿用上一条连接的窗口
         *          压这一条的连接（那等于把本端声明过的上限作废）。
         */
        struct ReusableDeflateStream
        {
            z_stream stream{};      ///< 复用的 deflate 流
            bool     isOpen{false}; ///< 是否已 init 且可复用（false 表示下次调用需重新 init）
            int      windowBits{0}; ///< 本流 init 时用的窗口位数（正数口径，init 时取负）

            ~ReusableDeflateStream()
            {
                // 线程退出时释放长期持有的 deflate 状态
                if (isOpen)
                {
                    ::deflateEnd(&stream);
                }
            }
        };

        /**
         * @brief 本线程复用的解压流与 16KiB 输出块缓冲，理由同 ReusableDeflateStream
         *        （含「位数变了必须重新 init」那一条）
         */
        struct ReusableInflateStream
        {
            z_stream    stream{};      ///< 复用的 inflate 流
            std::string chunk;         ///< 逐块解压用的暂存缓冲，容量跨消息保留
            bool        isOpen{false}; ///< 是否已 init 且可复用
            int         windowBits{0}; ///< 本流 init 时用的窗口位数

            ~ReusableInflateStream()
            {
                if (isOpen)
                {
                    ::inflateEnd(&stream);
                }
            }
        };

        /**
         * @brief 取下一个逗号分隔项并前移游标
         * @param text 全部分隔文本
         * @param offset 输入输出：当前游标
         * @return std::string_view 本项（已去首尾空白）
         */
        [[nodiscard]] std::string_view takeNextItem(const std::string_view text, std::size_t &offset)
        {
            const std::size_t commaPosition = text.find(',', offset);
            const std::size_t itemEnd       = commaPosition == std::string_view::npos ? text.size() : commaPosition;
            std::string_view  item          = text.substr(offset, itemEnd - offset);
            offset                          = itemEnd + 1;

            while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
            {
                item.remove_prefix(1);
            }
            while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
            {
                item.remove_suffix(1);
            }
            return item;
        }

        /**
         * @brief 取扩展参数列表里的下一项（参数之间以分号分隔，见 RFC 6455 §9.1 的扩展语法）
         * @param text 某个扩展名之后的参数文本
         * @param offset 输入输出：当前游标
         * @return std::string_view 本参数（已去首尾空白）
         */
        [[nodiscard]] std::string_view takeNextParameter(const std::string_view text, std::size_t &offset)
        {
            const std::size_t semicolonPosition = text.find(';', offset);
            const std::size_t parameterEnd      = semicolonPosition == std::string_view::npos ? text.size() : semicolonPosition;
            std::string_view  parameter         = text.substr(offset, parameterEnd - offset);
            offset                              = parameterEnd + 1;

            while (!parameter.empty() && (parameter.front() == ' ' || parameter.front() == '\t'))
            {
                parameter.remove_prefix(1);
            }
            while (!parameter.empty() && (parameter.back() == ' ' || parameter.back() == '\t'))
            {
                parameter.remove_suffix(1);
            }
            return parameter;
        }

        /**
         * @brief token 语义的相等判断（大小写不敏感、长度必须一致）
         * @param actual 收到的 token
         * @param expected 期望的 token（本身已是小写）
         * @return true 相等
         */
        [[nodiscard]] bool tokenEqualsIgnoringCase(const std::string_view actual, const std::string_view expected)
        {
            if (actual.size() != expected.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < expected.size(); ++index)
            {
                const char character = actual[index];
                const char lowered   = (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character;
                if (lowered != expected[index])
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief 判断一个扩展名是不是 permessage-deflate（token 语义，大小写不敏感）
         * @param extensionName 扩展名
         * @return true 是 permessage-deflate
         */
        [[nodiscard]] bool isPerMessageDeflateName(const std::string_view extensionName)
        {
            return tokenEqualsIgnoringCase(extensionName, "permessage-deflate");
        }

        /**
         * @brief 去掉参数值外围的双引号（扩展语法允许 quoted-string，见 RFC 6455 §9.1）
         * @param value 等号右侧的原始文本
         * @return std::string_view 去引号后的值；不成对的引号原样交回，交给下一步的数字判定
         */
        [[nodiscard]] std::string_view unquote(const std::string_view value)
        {
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            {
                return value.substr(1, value.size() - 2);
            }
            return value;
        }

        /**
         * @brief 解析窗口位数参数
         *
         * @details 只接受纯十进制且落在 RFC 7692 §7.1.1/§7.1.2 允许的那八档。带符号、带小数、
         *          非数字、越界一律判为「无法履约」：这些形状说明对端要么实现不对、要么在探本端，
         *          按缺省窗口继续开连接会把一条解不开的流发到线上。
         * @param rawValue 等号右侧已去引号的文本；空表示参数不带值（本端按 15 处理，另判）
         * @return std::optional<int> 合法位数；非法时为空
         */
        [[nodiscard]] std::optional<int> parseWindowBits(const std::string_view rawValue)
        {
            if (rawValue.empty())
            {
                return std::nullopt;
            }
            int value = 0;
            for (const char character: rawValue)
            {
                if (character < '0' || character > '9')
                {
                    return std::nullopt;
                }
                // 位数上限只有两位十进制，累加不会溢出；出现更多位就直接判越界
                value = value * 10 + (character - '0');
                if (value > kWebSocketDefaultWindowBits)
                {
                    return std::nullopt;
                }
            }
            if (value < kWebSocketMinimumWindowBits)
            {
                return std::nullopt;
            }
            return value;
        }
    } // namespace

    PerMessageDeflateNegotiation negotiatePerMessageDeflate(const std::string_view extensionsHeader)
    {
        PerMessageDeflateNegotiation negotiation;

        // 逐项扫描：头里可以并列多个扩展（RFC 6455 §9.1），只认 permessage-deflate 那一个
        std::size_t offset = 0;
        while (offset < extensionsHeader.size())
        {
            std::string_view item = takeNextItem(extensionsHeader, offset);
            if (item.empty())
            {
                continue;
            }

            const std::size_t      semicolonPosition = item.find(';');
            const std::string_view extensionName     = item.substr(0, semicolonPosition);
            if (!isPerMessageDeflateName(extensionName))
            {
                continue;
            }

            // 参数扫描：RFC 7692 §9.1 列出了「服务器必须婉拒这条要约」的几种形态，其中三条落在这里：
            // ① 要约里出现了本扩展没定义的参数名、② 同名参数重复出现、③ 取值不合法。
            // 要约方向只定义了四条（§7.1.1–§7.1.4）：两个窗口位数与两条 no_context_takeover。
            // 婉拒的形态是「101 里不回 Sec-WebSocket-Extensions」，连接照常按明文收发，因此严格化不会
            // 打断真客户端——浏览器与 aioquic 都不发这三类形态。
            int  compressBits    = kWebSocketDefaultWindowBits; // 本端压缩位数：受对端 server_max_window_bits 约束
            int  decompressBits  = kWebSocketDefaultWindowBits; // 本端解压位数：按对端 client_max_window_bits 收小
            bool hasServerWindow = false;                       ///< 对端是否提到了 server_max_window_bits（决定是否要回显）
            bool unusable        = false;                       ///< 遇到必须婉拒的形态：整条扩展不接受
            /// 各参数名在本次要约里出现的次数：同名重复即无从判断对端声明的到底是哪个配置
            int serverWindowOccurrences    = 0;
            int clientWindowOccurrences    = 0;
            int serverNoContextOccurrences = 0;
            int clientNoContextOccurrences = 0;

            std::size_t parameterOffset = semicolonPosition == std::string_view::npos ? item.size() : semicolonPosition + 1;
            while (parameterOffset < item.size() && !unusable)
            {
                const std::string_view parameter      = takeNextParameter(item, parameterOffset);
                const std::size_t      equalsPosition = parameter.find('=');
                const std::string_view parameterName  = parameter.substr(0, equalsPosition);
                const bool             hasValue       = equalsPosition != std::string_view::npos;
                const std::string_view rawValue       = hasValue ? unquote(parameter.substr(equalsPosition + 1)) : std::string_view{};

                const bool isServerWindow            = tokenEqualsIgnoringCase(parameterName, "server_max_window_bits");
                const bool isClientWindow            = tokenEqualsIgnoringCase(parameterName, "client_max_window_bits");
                const bool isServerNoContextTakeover = tokenEqualsIgnoringCase(parameterName, "server_no_context_takeover");
                // ①：不认识的名字不能当「与本端无关、忽略即可」放过——§9.1 把它列在必须婉拒的清单里，
                // 因为本端无从知道对端多出来的那条要求要不要履约
                if (!isServerWindow && !isClientWindow && !isServerNoContextTakeover && !tokenEqualsIgnoringCase(parameterName, "client_no_context_takeover"))
                {
                    unusable = true;
                    break;
                }

                if (isServerWindow)
                {
                    // ②：同名重复时「取最严的那个」是替对端猜，婉拒才是这条给的处置
                    if (++serverWindowOccurrences > 1)
                    {
                        unusable = true;
                        break;
                    }
                    hasServerWindow = true;
                    if (hasValue)
                    {
                        const std::optional<int> requested = parseWindowBits(rawValue);
                        if (!requested.has_value())
                        {
                            unusable = true;
                            break;
                        }
                        compressBits = std::min(compressBits, *requested);
                    }
                    // 不带值的形态（RFC 7692 §7.1.2）只表示「对端能开满 15」，本端无需收小
                } else if (isClientWindow)
                {
                    if (++clientWindowOccurrences > 1)
                    {
                        unusable = true;
                        break;
                    }
                    if (hasValue)
                    {
                        const std::optional<int> requested = parseWindowBits(rawValue);
                        if (!requested.has_value())
                        {
                            unusable = true;
                            break;
                        }
                        decompressBits = std::min(decompressBits, *requested);
                    }
                    // 本端不回显这一条：回显一个更低的值等于要求对端改小它的压缩窗口，而那要求
                    // 需要本端的实现来兜住对端不遵守的情形，收益只是内存——留给真有需求的一端
                } else if (isServerNoContextTakeover)
                {
                    // 这两条是布尔参数：带值即取值不合法（③）。识别即可——本端本来就按
                    // 「每条消息重置上下文」实现，而 §7.1.3/§7.1.4 明确允许回应里带它们即使对端没提
                    if (++serverNoContextOccurrences > 1 || hasValue)
                    {
                        unusable = true;
                        break;
                    }
                } else
                {
                    if (++clientNoContextOccurrences > 1 || hasValue)
                    {
                        unusable = true;
                        break;
                    }
                }
            }

            if (unusable)
            {
                // 无法履约就整条扩展不接受：101 里不回 Sec-WebSocket-Extensions，对端按明文收发，
                // 连接照常可用。带着一个对端解不开的窗口把连接开起来才是最坏的一种「看起来成功了」
                return negotiation;
            }

            // 接受并回本端选定的参数：两条 no_context_takeover 都要求「每条消息重置上下文」，
            // 于是收发两侧都不必保存跨消息的 z_stream（本端实现细节里写明这笔取舍）
            negotiation.accepted = true;
            negotiation.window   = PerMessageDeflateWindow{.compressBits = compressBits, .decompressBits = decompressBits};

            std::string responseValue = "permessage-deflate; server_no_context_takeover; client_no_context_takeover";
            if (hasServerWindow)
            {
                // 对端提过这个参数就把它选定的位数回过去（RFC 7692 §7.1.2 的应答形态）：
                // 对端据此分配自己的解压窗口，本端随后确实按这个数压
                responseValue += "; server_max_window_bits=" + std::to_string(compressBits);
            }
            negotiation.responseValue = std::move(responseValue);
            return negotiation;
        }

        return negotiation;
    }

    std::optional<std::string> deflateWebSocketMessage(const std::string_view payload, const int windowBits)
    {
        if (windowBits < kWebSocketMinimumWindowBits || windowBits > kWebSocketDefaultWindowBits)
        {
            return std::nullopt;
        }

        // 复用本线程的 deflate 流：见 ReusableDeflateStream 注释——no_context_takeover 下每条消息独立，
        // deflateReset 即等价于「新建一条流」，却免去约 200KB 内部状态的反复重建
        thread_local ReusableDeflateStream context;
        if (!context.isOpen || context.windowBits != windowBits)
        {
            // 位数与上次 init 的不同：这条线程正在换一条连接服务，窗口必须跟着新连接的协商走
            if (context.isOpen)
            {
                ::deflateEnd(&context.stream);
                context.isOpen = false;
                context.stream = z_stream{};
            }
            // 裸 deflate 的 windowBits 取负值：不带 zlib 头尾，直接产 RFC 1951 字节流
            if (::deflateInit2(&context.stream, kWebSocketDeflateLevel, Z_DEFLATED, -windowBits, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            {
                context.windowBits = 0;
                return std::nullopt;
            }
            context.isOpen     = true;
            context.windowBits = windowBits;
        }

        z_stream &stream = context.stream;
        // 每次使用前复位到「刚 init」的状态；复位失败说明流已不可信，关掉让下次重新 init
        if (::deflateReset(&stream) != Z_OK)
        {
            ::deflateEnd(&stream);
            context.isOpen = false;
            return std::nullopt;
        }

        // 输入只有消息本身：RFC 7692 §7.2.1 的「消息后接四字节尾」不是让调用方手工拼进输入，
        // 而是 Z_SYNC_FLUSH 的既有行为——它总会以空块收尾，其最后四字节正是 00 00 FF FF。
        // 因此这里压完再把输出末尾那四字节去掉，解压侧自行补回，两侧互为逆运算。
        // 若把尾字节当数据喂进去，它会被当成消息内容压进输出，解压后凭空多出四字节
        const std::string_view input = payload;

        // 输出缓冲按「输入 + 余量」给：deflateBound() 给的是 Z_FINISH 一次压完的上界，
        // 而这里用的是 Z_SYNC_FLUSH（尾部要多一个空块），踩在它的边界上会得到「缓冲写满、
        // 尾部没写出来」的结果。数据量本来就不大，直接给足余量比抠上界划算
        std::string output(input.size() + kDeflateOutputHeadroomBytes, '\0');
        stream.next_in   = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
        stream.avail_in  = static_cast<uInt>(input.size());
        stream.next_out  = reinterpret_cast<Bytef *>(output.data());
        stream.avail_out = static_cast<uInt>(output.size());

        // Z_SYNC_FLUSH 而不是 Z_FINISH：前者产出的尾部正是可被截掉、也可被解压侧补回的空块，
        // 后者会写终结块，截掉四字节后剩下的字节流解压侧补尾也解不开。
        // 循环到「输入吃光且本轮没写满缓冲」为止：写满就说明还有待写出的字节
        std::size_t producedBytes = 0;
        while (true)
        {
            const int result = ::deflate(&stream, Z_SYNC_FLUSH);
            if (result != Z_OK && result != Z_BUF_ERROR && result != Z_STREAM_END)
            {
                ::deflateEnd(&stream);
                context.isOpen = false;
                return std::nullopt;
            }

            producedBytes = output.size() - stream.avail_out;
            if (stream.avail_in == 0 && stream.avail_out > 0)
            {
                break;
            }
            if (result == Z_BUF_ERROR && stream.avail_out == 0)
            {
                // 缓冲真的不够（理论上有余量就不该发生）：放弃压缩而不是发出半截流
                ::deflateEnd(&stream);
                context.isOpen = false;
                return std::nullopt;
            }
        }
        // 成功路径不再 deflateEnd：把流留给下一条消息 reset 复用（线程退出时由析构释放）

        // 输出必须真的以那四字节收尾：不是的话说明 zlib 行为与预期不符，宁可放弃压缩也不要发出
        // 一段对端解不开的负载；此刻流状态存疑，关掉让下次重新 init
        if (producedBytes < kDeflateTail.size() || std::memcmp(output.data() + producedBytes - kDeflateTail.size(), kDeflateTail.data(), kDeflateTail.size()) != 0)
        {
            ::deflateEnd(&stream);
            context.isOpen = false;
            return std::nullopt;
        }

        output.resize(producedBytes - kDeflateTail.size());
        return output;
    }

    std::optional<std::string> inflateWebSocketMessage(const std::string_view payload, const std::size_t maximumOutputBytes, const int windowBits)
    {
        if (windowBits < kWebSocketMinimumWindowBits || windowBits > kWebSocketDefaultWindowBits)
        {
            return std::nullopt;
        }

        // 与压缩侧对称：复用本线程的 inflate 流与块缓冲，每条消息前 inflateReset；
        // 位数变了同样要重新 init（见 ReusableInflateStream 注释）
        thread_local ReusableInflateStream context;
        if (!context.isOpen || context.windowBits != windowBits)
        {
            if (context.isOpen)
            {
                ::inflateEnd(&context.stream);
                context.isOpen = false;
                context.stream = z_stream{};
            }
            if (::inflateInit2(&context.stream, -windowBits) != Z_OK)
            {
                context.windowBits = 0;
                return std::nullopt;
            }
            context.isOpen     = true;
            context.windowBits = windowBits;
        }

        z_stream &stream = context.stream;
        if (::inflateReset(&stream) != Z_OK)
        {
            ::inflateEnd(&stream);
            context.isOpen = false;
            return std::nullopt;
        }

        std::string input;
        input.reserve(payload.size() + kDeflateTail.size());
        input.append(payload);
        input.append(kDeflateTail.data(), kDeflateTail.size());

        stream.next_in  = reinterpret_cast<Bytef *>(input.data());
        stream.avail_in = static_cast<uInt>(input.size());

        std::string output;
        // 块缓冲容量跨消息保留，省掉每条消息重填 16KiB；resize 到固定块大小供本轮写入
        context.chunk.resize(kInflateChunkBytes);
        std::string &chunk = context.chunk;

        while (true)
        {
            stream.next_out  = reinterpret_cast<Bytef *>(chunk.data());
            stream.avail_out = static_cast<uInt>(chunk.size());

            const int result = ::inflate(&stream, Z_SYNC_FLUSH);
            if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR)
            {
                ::inflateEnd(&stream);
                context.isOpen = false;
                return std::nullopt;
            }

            const std::size_t producedBytes = chunk.size() - stream.avail_out;
            // 上限在累加之前判：压缩比可以做到几百倍，先累加再判等于先把内存吃下去
            if (maximumOutputBytes != 0 && output.size() + producedBytes > maximumOutputBytes)
            {
                ::inflateEnd(&stream);
                context.isOpen = false;
                return std::nullopt;
            }
            output.append(chunk.data(), producedBytes);

            // 输入吃完且本轮没能再产出，或已到流末尾：两种情况都没有更多可解的内容
            if (result == Z_STREAM_END || (stream.avail_in == 0 && producedBytes < chunk.size()))
            {
                break;
            }
            if (result == Z_BUF_ERROR && stream.avail_in == 0)
            {
                break;
            }
        }
        // 成功路径不再 inflateEnd：留给下一条消息 reset 复用

        return output;
    }
} // namespace AsynGyanis::Net
