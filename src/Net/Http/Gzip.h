/**
 * @file Gzip.h
 * @brief gzip（RFC 1952）压缩：响应压缩的最小构件
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    /// 默认压缩级别：zlib 的 6 是压缩率与 CPU 的常用折中（1 最快、9 最小）
    inline constexpr int kDefaultGzipLevel = 6;

    /**
     * @brief 用 gzip 容器压缩一段字节
     *
     * @details 走 zlib 的一次性 deflate：整段进、整段出。选择一次性而不是流式，是因为响应正文
     *          在本框架里本来就是完整的一块（业务写完才发），没有边压边发的需求；流式压缩要维护
     *          一个跨次调用的 z_stream，代价与复杂度都落在没人需要的地方。
     * @param input 待压缩内容，可为空
     * @param level 压缩级别，1..9；越界时由 zlib 自行夹取（0 表示不压缩，本函数不接受）
     * @return std::optional<std::string> 压缩后的 gzip 字节；zlib 初始化或压缩失败（通常是内存不足）
     *         时返回空，调用方据此退回「不压缩」而不是发一个坏掉的响应
     * @note 压缩后可能比原文更大（小内容、已压缩内容）：是否值得压缩由调用方按阈值决定，
     *       本函数只负责压，不做策略
     * @see compressionMiddleware()
     */
    [[nodiscard]] std::optional<std::string> gzipCompress(std::string_view input, int level = kDefaultGzipLevel);
} // namespace AsynGyanis::Net
