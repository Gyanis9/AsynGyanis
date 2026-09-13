#include "Net/Http/Gzip.h"

#include <zlib.h>

#include <cstddef>
#include <cstring>

namespace AsynGyanis::Net
{
    std::optional<std::string> gzipCompress(const std::string_view input, const int level)
    {
        // gzip 容器而不是裸 deflate：windowBits 加上 16 就是「输出 gzip 头与尾」（RFC 1952），
        // 这样对端拿到的是标准的 Content-Encoding: gzip 字节，curl/browser 直接能解
        z_stream stream{};
        if (::deflateInit2(&stream, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        {
            return std::nullopt;
        }

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
            return std::nullopt;
        }

        output.resize(static_cast<std::size_t>(stream.total_out));
        ::deflateEnd(&stream);
        return output;
    }
} // namespace AsynGyanis::Net
