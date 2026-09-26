#include "Net/Http/Compression.h"

#include <brotli/encode.h>
#include <zstd.h>

#include <algorithm>
#include <cstdint>

namespace AsynGyanis::Net
{
    std::optional<std::string> zstdCompress(const std::string_view input, const int level)
    {
        // 空输入时 data() 可能是空指针，而 zstd 的 src 参数要求有效指针（哪怕长度为 0）
        static constexpr char kEmptyInput[] = "";
        const char *const     source        = input.empty() ? kEmptyInput : input.data();

        // 复用 thread_local 压缩上下文：ZSTD_compress 每次调用都内部新建并销毁一个 ZSTD_CCtx（含数百 KB
        // 工作区），与 permessage-deflate 侧重建 deflate 状态同源的浪费——每条响应一次，短正文上固定开销占比很高。
        // ZSTD_compressCCtx 用外部上下文做单段压缩、每次自行复位并按入参应用压缩级别，产出与 ZSTD_compress 逐字节一致。
        // thread_local 使每条循环线程各持一份，天然无跨线程共享；上下文创建失败（OOM）退回不压缩语义。
        struct ReusableZstdContext
        {
            ZSTD_CCtx *const context{::ZSTD_createCCtx()}; ///< 复用的压缩上下文，创建失败为空
            ~ReusableZstdContext()
            {
                if (context != nullptr)
                {
                    ::ZSTD_freeCCtx(context);
                }
            }
        };
        thread_local ReusableZstdContext holder;
        if (holder.context == nullptr)
        {
            return std::nullopt;
        }

        std::string       output(ZSTD_compressBound(input.size()), '\0');
        const std::size_t writtenLength = ::ZSTD_compressCCtx(holder.context, output.data(), output.size(), source, input.size(), level);
        if (ZSTD_isError(writtenLength) != 0)
        {
            return std::nullopt;
        }

        output.resize(writtenLength);
        return output;
    }

    std::optional<std::string> brotliCompress(const std::string_view input, const int quality)
    {
        // 空输入时 MaxCompressedSize(0) 返回 0：给一块 1 字节的合法缓冲，别让编码器往空缓冲写
        static constexpr char kEmptyInput[] = "";
        const auto *const     source        = reinterpret_cast<const std::uint8_t *>(input.empty() ? kEmptyInput : input.data());

        const std::size_t maximumSize = BrotliEncoderMaxCompressedSize(input.size());
        std::string       output(maximumSize == 0 ? 1 : maximumSize, '\0');

        // 质量越界由本函数夹取：BrotliEncoderCompress 对非法参数直接返回失败，而调用方
        // 传 7 还是 70 都只是「想要更高的压缩率」，不该让整个压缩被拒
        const int   clampedQuality = std::clamp(quality, 0, BROTLI_MAX_QUALITY);
        std::size_t writtenLength  = output.size();
        if (BrotliEncoderCompress(clampedQuality, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_GENERIC, input.size(), source, &writtenLength,
                                  reinterpret_cast<std::uint8_t *>(output.data())) == BROTLI_FALSE)
        {
            return std::nullopt;
        }

        output.resize(writtenLength);
        return output;
    }
} // namespace AsynGyanis::Net
