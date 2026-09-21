#include "Net/Http/Gzip.h"

#include <zlib.h>

#include <cstddef>
#include <cstring>

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
            z_stream stream{};         ///< 复用的 deflate 流
            bool isOpen{false};        ///< 是否已 init 且可复用
            int level{0};              ///< 建流时用的压缩级别，与入参不符则重建

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
            }
            else
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
        return output;   // 成功路径不 End：留给下一条响应 deflateReset 复用（线程退出时由析构释放）
    }
} // namespace AsynGyanis::Net
