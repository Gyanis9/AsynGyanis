#include "Net/Http/HttpChunkFrame.h"

#include "Base/Exception/LogicException.h"

#include <charconv>
#include <format>
#include <string>
#include <system_error>

namespace AsynGyanis::Net
{
    std::string_view chunkFramePayload(const std::string_view chunkFrame)
    {
        // 长度行到第一个 CRLF 为止，位数不会超过一个 size_t 的十六进制位数
        const std::size_t lengthLineEndIndex = chunkFrame.find(kChunkFrameCrLf);
        if (lengthLineEndIndex == std::string_view::npos || lengthLineEndIndex == 0 ||
            lengthLineEndIndex > kChunkLengthLineMaximumLength)
        {
            throw Base::LogicException("Net: writeChunk 交出的分块帧没有合法的长度行（应为「<十六进制字节数>\\r\\n」），"
                                       "本段未发出；请检查 HttpResponse::writeChunk() 的实现与其文档是否一致");
        }

        // 长度前缀是负载长度的唯一权威来源：正文里出现 CRLF 也不影响边界判定
        std::size_t payloadLength = 0;
        const auto [parseEnd, parseError] =
                std::from_chars(chunkFrame.data(), chunkFrame.data() + lengthLineEndIndex, payloadLength, 16);
        if (parseError != std::errc() || parseEnd != chunkFrame.data() + lengthLineEndIndex)
        {
            throw Base::LogicException("Net: 分块帧的长度行不是合法的十六进制字节数（「" +
                                       std::string(chunkFrame.substr(0, lengthLineEndIndex)) + "」），本段未发出；"
                                       "请检查 HttpResponse::writeChunk() 的实现与其文档是否一致");
        }

        // 长度行 + 负载 + 结尾 CRLF 必须恰好用满整段字节：多一个字节或少一个都说明帧布局与文档不符
        if (chunkFrame.size() != lengthLineEndIndex + kChunkFrameCrLf.size() + payloadLength + kChunkFrameCrLf.size() ||
            chunkFrame.compare(chunkFrame.size() - kChunkFrameCrLf.size(), kChunkFrameCrLf.size(), kChunkFrameCrLf) != 0)
        {
            throw Base::LogicException(std::format("Net: 分块帧的实际长度 {} 字节与长度行声明的 {} 字节不一致，本段未发出；"
                                                   "请检查 HttpResponse::writeChunk() 的实现与其文档是否一致",
                                                   chunkFrame.size(), payloadLength));
        }
        return chunkFrame.substr(lengthLineEndIndex + kChunkFrameCrLf.size(), payloadLength);
    }
} // namespace AsynGyanis::Net
