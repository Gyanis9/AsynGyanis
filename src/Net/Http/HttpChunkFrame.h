/**
 * @file HttpChunkFrame.h
 * @brief 从 HttpResponse::writeChunk() 交出的 HTTP/1.1 分块帧里剥出应用负载
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    /// 分块帧里的行分隔符（RFC 9112 §7.1）
    inline constexpr std::string_view kChunkFrameCrLf = "\r\n";

    /// 长度行允许的最大长度：十六进制位数不会超过一个 size_t 的位数
    inline constexpr std::size_t kChunkLengthLineMaximumLength = sizeof(std::size_t) * 2U;

    /**
     * @brief 剥出一帧 HTTP/1.1 分块帧里的应用负载
     *
     * @details `HttpResponse::writeChunk()` 是 h1 口径的接口：它交出的字节已经是 h1 线格式
     *          （`<十六进制长度>\r\n<数据>\r\n`，RFC 9112 §7.1）。h2/h3 里都没有「分块帧」这一层
     *          （transfer-encoding 在 h2 里是禁止头，h3 更不存在这个概念），所以两种承载都要先把
     *          负载剥出来，再各自发成 DATA 帧。这段剥离逻辑因此在两个会话之间共用，不各写一份。
     *
     * @param chunkFrame 一段分块帧原文
     * @return std::string_view 帧里的应用负载（指向入参内存，寿命随入参）
     * @throws Base::LogicException 帧布局与 `HttpResponse::writeChunk()` 的约定不符：没有合法长度行、
     *         长度行不是十六进制数、或实际字节数与声明长度对不上。此时宁可当场报错，也绝不把帧头或
     *         残缺负载当成正文发出去
     */
    [[nodiscard]] std::string_view chunkFramePayload(std::string_view chunkFrame);

    /**
     * @brief 把一段应用负载写成 h1 的分块帧，写进调用方持有的缓冲
     * @details 帧 = `<十六进制长度>\r\n<数据>\r\n`（RFC 9112 §7.1），与 chunkFramePayload() 严格互逆。
     *          缓冲交给调用方跨次复用：正文段的大小通常稳定，每段都新建一个串等于在每个事件上摊
     *          「一次分配 + 一次整段拷贝」，SSE 这类小段高频出口最明显。本函数清空缓冲但不缩容量。
     * @param frame 输出缓冲；成功返回时长度恰好是一帧
     * @param data 本段应用负载，非空（零长度块是终止块语义，跳过空段由调用方负责）
     * @throws Base::LogicException 长度写不成十六进制文本；此时 frame 保持原样
     */
    void appendChunkFrame(std::string &frame, std::string_view data);
} // namespace AsynGyanis::Net
