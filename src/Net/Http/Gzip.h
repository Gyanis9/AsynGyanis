/**
 * @file Gzip.h
 * @brief gzip（RFC 1952）压缩：响应压缩的最小构件
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    /// 默认压缩级别：zlib 的 6 是压缩率与 CPU 的常用折中（1 最快、9 最小）
    inline constexpr int kDefaultGzipLevel = 6;

    /// 解出来的正文默认上界：8 MiB，与 HttpParserLimits::maximumBodySize 同档
    inline constexpr std::size_t kDefaultInflateOutputLimitBytes = 8U * 1024U * 1024U;

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

    /**
     * @brief 解一段 deflate 家族的压缩正文（gzip 容器或 zlib 流都认）
     *
     * @details 建流用 windowBits = 15 + 32，即 zlib 的「自动识别」档：gzip 头（RFC 1952）与 zlib 头
     *          （RFC 1950）都能解。这不是宽容过头——HTTP 里 `Content-Encoding: deflate` 规范上指
     *          zlib 流，而线上确实有服务端标 deflate 却发 gzip、或反过来标 gzip 发 zlib；两种容器
     *          都由同一份 inflate 处理，省下的是「按标签建流然后解不出」那一类真实故障。
     *          裸 deflate（无头，raw）不在放行范围内：那种字节没有自描述，猜窗口位是拿正确性换兼容。
     *          注意 zlib 那边「只认 gzip」是 +16、「两种都认」才是 +32：写错不会报错，只会让
     *          带 zlib 头的响应统统判成坏数据。
     *
     * @warning 输出上界是必需项而不是可选项：gzip 压缩比可以做到上千倍，一小块「zip 炸弹」正文
     *          就能把内存吃光。到界即判失败（而不是交回前 N 字节）——交回半截正文等于把损坏藏起来，
     *          调用方看到的是一份长度对、内容却错了的数据。
     *
     * @param input 压缩字节，可为空（空输入按「解出空正文」处理，与 0 长度的 gzip 流不同：后者仍要
     *              带上容器头与校验，空 input 直接判格式错误）
     * @param maxOutputByteCount 解出来的正文长度上界，单位字节
     * @return std::expected<std::string, std::string> 成功为解出的正文；失败为可直接进日志的中文原因
     * @see HttpContentCoding.h 的按 Content-Encoding 分派
     */
    [[nodiscard]] std::expected<std::string, std::string> inflateHttpBody(std::string_view input, std::size_t maxOutputByteCount = kDefaultInflateOutputLimitBytes);
} // namespace AsynGyanis::Net
