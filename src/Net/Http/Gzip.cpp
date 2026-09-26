#include "Net/Http/Gzip.h"

#include <zlib.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

namespace AsynGyanis::Net
{
    std::optional<std::string> gzipCompress(const std::string_view input, const int level)
    {
        // 复用 thread_local 的 deflate 流：gzipCompress 每条压缩响应都 deflateInit2/End，重建约 200KB+ 的
        // 压缩器内部状态，与 permessage-deflate 侧同源的固定开销浪费。每条前 deflateReset 复位即可复用，
        // 产出的 gzip 帧与「每条新建一条流」逐字节一致。压缩级别是运行期参数但同一服务器实际恒定，
        // 故仅在级别变化时重建流（罕见）；出错路径就地 End 并标记关闭，下次重新 init，绝不在可疑状态续用。
        struct ReusableGzipDeflater
        {
            z_stream stream{};      ///< 复用的 deflate 流
            bool     isOpen{false}; ///< 是否已 init 且可复用
            int      level{0};      ///< 建流时用的压缩级别，与入参不符则重建

            ~ReusableGzipDeflater()
            {
                if (isOpen)
                {
                    ::deflateEnd(&stream);
                }
            }
        };
        thread_local ReusableGzipDeflater context;

        if (context.isOpen)
        {
            if (context.level == level)
            {
                // 级别不变：复位到「刚 init」状态复用。复位失败说明流不可信，关掉让下面重新 init
                if (::deflateReset(&context.stream) != Z_OK)
                {
                    ::deflateEnd(&context.stream);
                    context.isOpen = false;
                }
            } else
            {
                ::deflateEnd(&context.stream);
                context.isOpen = false;
            }
        }
        if (!context.isOpen)
        {
            // gzip 容器而不是裸 deflate：windowBits 加上 16 就是「输出 gzip 头与尾」（RFC 1952），
            // 这样对端拿到的是标准的 Content-Encoding: gzip 字节，curl/browser 直接能解
            if (::deflateInit2(&context.stream, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            {
                return std::nullopt;
            }
            context.isOpen = true;
            context.level  = level;
        }

        z_stream &stream = context.stream;
        // 输出缓冲按上界一次要够：deflateBound 给的是「最坏情况」的字节数，
        // 因此循环里不会因为空间不足反复扩容，也就不必处理 Z_OK 的中间态
        const uLong boundBytes = ::deflateBound(&stream, static_cast<uLong>(input.size()));
        std::string output(static_cast<std::size_t>(boundBytes), '\0');

        stream.next_in   = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
        stream.avail_in  = static_cast<uInt>(input.size());
        stream.next_out  = reinterpret_cast<Bytef *>(output.data());
        stream.avail_out = static_cast<uInt>(output.size());

        // 一次性压完：业务写完整个响应才发送，没有边压边发的场景
        const int result = ::deflate(&stream, Z_FINISH);
        if (result != Z_STREAM_END)
        {
            ::deflateEnd(&stream);
            context.isOpen = false;
            return std::nullopt;
        }

        output.resize(static_cast<std::size_t>(stream.total_out));
        return output; // 成功路径不 End：留给下一条响应 deflateReset 复用（线程退出时由析构释放）
    }

    std::expected<std::string, std::string> inflateHttpBody(const std::string_view input, const std::size_t maxOutputByteCount)
    {
        // 与压缩侧同一套复用手法：inflate 流的内部窗口字典有 32 KiB，每条响应都重建就是白付的固定开销。
        // inflateReset 把流复位到「刚 init」状态，字典与已分配的空间都留着
        struct ReusableInflater
        {
            z_stream stream{};      ///< 复用的 inflate 流
            bool     isOpen{false}; ///< 是否已 init 且可复用

            ~ReusableInflater()
            {
                if (isOpen)
                {
                    ::inflateEnd(&stream);
                }
            }
        };
        thread_local ReusableInflater context;

        if (input.empty())
        {
            return std::unexpected("压缩正文为空：0 字节不是一条合法的 deflate 流，请检查 Content-Encoding 与正文是否匹配");
        }

        if (context.isOpen && ::inflateReset(&context.stream) != Z_OK)
        {
            // 复位失败说明流处在不可信的状态：就地关掉，下面重新 init，绝不在可疑状态续用
            ::inflateEnd(&context.stream);
            context.isOpen = false;
        }
        if (!context.isOpen)
        {
            // windowBits 取 15 + 32 = 「自动识别 gzip 头或 zlib 头」（zlib 的这条是 32 而不是 16：
            // 加 16 只认 gzip，加 32 才两种都认——见头文件里的理由）；
            // 裸 deflate（负 windowBits）刻意不放行，那种字节没有自描述，猜格式等于拿正确性换兼容
            if (::inflateInit2(&context.stream, 15 + 32) != Z_OK)
            {
                return std::unexpected("zlib 解压器初始化失败（通常是内存不足）");
            }
            context.isOpen = true;
        }

        z_stream &stream = context.stream;
        stream.next_in   = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
        stream.avail_in  = static_cast<uInt>(input.size());

        // 输出侧只能边解边长：压缩比在几十到上千倍之间，事前按 input × 系数一次要够要么浪费、
        // 要么仍不够。翻倍扩到上界为止，上界是必需项（见头文件的 zip 炸弹说明）
        const std::size_t initialChunkByteCount = std::max<std::size_t>(input.size() * 4U, 4096U);
        std::string       output;
        output.resize(std::min(initialChunkByteCount, maxOutputByteCount));
        std::size_t writtenByteCount = 0;

        while (true)
        {
            stream.next_out  = reinterpret_cast<Bytef *>(output.data() + writtenByteCount);
            stream.avail_out = static_cast<uInt>(output.size() - writtenByteCount);

            const int result = ::inflate(&stream, Z_NO_FLUSH);
            writtenByteCount += output.size() - writtenByteCount - stream.avail_out;
            if (result == Z_STREAM_END)
            {
                output.resize(writtenByteCount);
                return output; // 成功路径不 End：留给下一条响应 inflateReset 复用
            }
            if (result != Z_OK)
            {
                return std::unexpected(result == Z_DATA_ERROR ? std::string("压缩正文损坏：deflate 流校验失败（数据不全、被截断，或对端发的不是 deflate）")
                                                              : std::string("zlib 解压失败，错误码 ") + std::to_string(result));
            }

            if (stream.avail_out != 0U)
            {
                // 输出缓冲没写满却还没到头：说明输入先耗尽了，也就是流被截断。
                // 这时交回已解出的部分等于把「少了一截」伪装成正常正文，必须报错
                return std::unexpected("压缩正文不完整：输入已读完但 deflate 流还没到头（响应被截断？）");
            }
            if (output.size() >= maxOutputByteCount)
            {
                return std::unexpected("解出的正文超过上限 " + std::to_string(maxOutputByteCount) + " 字节：压缩比过高属于异常，按失败处理而不是交回半截正文");
            }

            // 写满且还在正常出字节：扩一段继续解。翻倍一次到位，避免每 4 KiB 一次 realloc
            output.resize(std::min(maxOutputByteCount, output.size() * 2U));
        }
    }
} // namespace AsynGyanis::Net
