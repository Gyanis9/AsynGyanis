/**
 * @file Compression.h
 * @brief zstd 与 brotli 响应压缩构件（与 Gzip.h 同一形态：整段进、整段出）
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    /// 默认 zstd 级别：取库自身的平衡点（ZSTD_CLEVEL_DEFAULT = 3），1 最快、22 最小
    inline constexpr int kDefaultZstdLevel = 3;

    /// 默认 brotli 质量：6 与 gzip 的 level 6 大致同档；11 面向静态预压缩，CPU 代价明显更高
    inline constexpr int kDefaultBrotliQuality = 6;

    /**
     * @brief 用 zstd 压缩一段字节
     *
     * @details 与 gzipCompress() 同一取舍：走一次性压缩（整段进、整段出），不做流式；
     *          级别越界由 zstd 自行夹取到合法区间，调用方不必先校验。
     * @param input 待压缩内容，可为空
     * @param level 压缩级别，1..22；越界自动夹取
     * @return std::optional<std::string> 压缩后的 zstd 帧；压缩失败（内存不足）时返回空，
     *         调用方据此退回「不压缩」而不是发一个坏掉的响应
     * @note 压缩后可能比原文更大（小内容、已压缩内容）：是否值得压缩由调用方按阈值决定
     * @see compressionMiddleware()
     */
    [[nodiscard]] std::optional<std::string> zstdCompress(std::string_view input, int level = kDefaultZstdLevel);

    /**
     * @brief 用 brotli 压缩一段字节
     *
     * @details 同上：一次性压缩、失败返回空。质量越界夹取到 brotli 的合法区间（0..11）。
     * @param input 待压缩内容，可为空
     * @param quality 压缩质量，0..11；越界自动夹取
     * @return std::optional<std::string> 压缩后的 brotli 流；压缩失败时返回空
     * @see compressionMiddleware()
     */
    [[nodiscard]] std::optional<std::string> brotliCompress(std::string_view input, int quality = kDefaultBrotliQuality);
} // namespace AsynGyanis::Net
