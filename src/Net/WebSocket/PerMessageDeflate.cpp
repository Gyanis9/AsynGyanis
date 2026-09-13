#include "Net/WebSocket/PerMessageDeflate.h"

#include <zlib.h>

#include <array>
#include <cstring>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 每条压缩消息末尾的固定四字节空块尾（RFC 7692 §7.2.1）：线上负载不含它，收发两侧各自增删
        constexpr std::array<char, 4> kDeflateTail{'\x00', '\x00', '\xFF', '\xFF'};

        /// 裸 deflate 的 windowBits 取负值：不带 zlib 头尾，直接产 RFC 1951 字节流
        constexpr int kRawDeflateWindowBits = -15;

        /// 逐块解压时的块大小
        constexpr std::size_t kInflateChunkBytes = 16 * 1024;

        /// 压缩输出缓冲相对输入的余量：同步刷新的空块最多几字节，给 64 足够且不占什么内存
        constexpr std::size_t kDeflateOutputHeadroomBytes = 64;

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
         * @brief 判断一个扩展名是不是 permessage-deflate（token 语义，大小写不敏感）
         * @param extensionName 扩展名
         * @return true 是 permessage-deflate
         */
        [[nodiscard]] bool isPerMessageDeflateName(const std::string_view extensionName)
        {
            constexpr std::string_view kExpectedName = "permessage-deflate";
            if (extensionName.size() != kExpectedName.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < kExpectedName.size(); ++index)
            {
                const char actual = extensionName[index];
                const char lowered = (actual >= 'A' && actual <= 'Z') ? static_cast<char>(actual - 'A' + 'a') : actual;
                if (lowered != kExpectedName[index])
                {
                    return false;
                }
            }
            return true;
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

            const std::size_t semicolonPosition = item.find(';');
            const std::string_view extensionName = item.substr(0, semicolonPosition);
            if (!isPerMessageDeflateName(extensionName))
            {
                continue;
            }

            // 接受并回本端选定的参数：两条 no_context_takeover 都要求「每条消息重置上下文」，
            // 于是收发两侧都不必保存跨消息的 z_stream（本端实现细节里写明这笔取舍）
            negotiation.accepted      = true;
            negotiation.responseValue = "permessage-deflate; server_no_context_takeover; client_no_context_takeover";
            return negotiation;
        }

        return negotiation;
    }

    std::optional<std::string> deflateWebSocketMessage(const std::string_view payload)
    {
        z_stream stream{};
        if (::deflateInit2(&stream, kWebSocketDeflateLevel, Z_DEFLATED, kRawDeflateWindowBits, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        {
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
                return std::nullopt;
            }
        }

        ::deflateEnd(&stream);

        // 输出必须真的以那四字节收尾：不是的话说明 zlib 行为与预期不符，宁可放弃压缩也不要发出
        // 一段对端解不开的负载
        if (producedBytes < kDeflateTail.size() ||
            std::memcmp(output.data() + producedBytes - kDeflateTail.size(), kDeflateTail.data(), kDeflateTail.size()) != 0)
        {
            return std::nullopt;
        }

        output.resize(producedBytes - kDeflateTail.size());
        return output;
    }

    std::optional<std::string> inflateWebSocketMessage(const std::string_view payload, const std::size_t maximumOutputBytes)
    {
        z_stream stream{};
        if (::inflateInit2(&stream, kRawDeflateWindowBits) != Z_OK)
        {
            return std::nullopt;
        }

        std::string input;
        input.reserve(payload.size() + kDeflateTail.size());
        input.append(payload);
        input.append(kDeflateTail.data(), kDeflateTail.size());

        stream.next_in  = reinterpret_cast<Bytef *>(input.data());
        stream.avail_in = static_cast<uInt>(input.size());

        std::string output;
        std::string chunk(kInflateChunkBytes, '\0');

        while (true)
        {
            stream.next_out  = reinterpret_cast<Bytef *>(chunk.data());
            stream.avail_out = static_cast<uInt>(chunk.size());

            const int result = ::inflate(&stream, Z_SYNC_FLUSH);
            if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR)
            {
                ::inflateEnd(&stream);
                return std::nullopt;
            }

            const std::size_t producedBytes = chunk.size() - stream.avail_out;
            // 上限在累加之前判：压缩比可以做到几百倍，先累加再判等于先把内存吃下去
            if (maximumOutputBytes != 0 && output.size() + producedBytes > maximumOutputBytes)
            {
                ::inflateEnd(&stream);
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

        ::inflateEnd(&stream);
        return output;
    }
} // namespace AsynGyanis::Net
