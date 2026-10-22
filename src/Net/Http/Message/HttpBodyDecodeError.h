/**
 * @file HttpBodyDecodeError.h
 * @brief 压缩正文解码（gzip / deflate / Content-Encoding 分派）的失败类别与中文可操作文案
 * @author Gyanis
 * @date 2026-10-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 类别按「调用方拿到之后做什么」划分：对端畸形的要按 400 收，超上限的要按 413 收，本端资源起不来的
 *          不能算客户端错。文案会改，分类才是契约。
 */

#pragma once

#include "AsynGyanisExport.h"

#include <expected>
#include <string>

namespace AsynGyanis::Net
{
    /**
     * @brief 压缩正文解不开的类别
     *
     * @details 新增类别一律追加在末尾：取值会进日志与指标，中间插入会让既有取值的含义整体位移。
     *          每一格都必须有真实产出点，加一个没人产生的类别等于给调用方一个永远进不去的分支。
     */
    enum class HttpBodyDecodeErrorKind
    {
        EmptyInput,                 ///< 压缩正文是 0 字节：不是一条合法容器，属对端畸形（声明了编码却没有内容）
        CorruptStream,              ///< deflate 流校验不过（数据被改、字典不合、发的不是压缩流）：对端畸形
        TruncatedStream,            ///< 输入读完而流还没到头（响应被截断）：对端畸形，交回半截正文等于伪装正常数据
        DecompressedSizeExceeded,   ///< 解出的正文超过调用方给定的上限：压缩比过高属异常负载，按「太大」而不是「格式错」处置
        UnsupportedChainedEncoding, ///< 对端声明了链式 Content-Encoding（如 `gzip, br`）：本端只处理单层，需对端改发或本端补解码器
        UnrequestedEncoding,        ///< 对端回了本端没请求的 Content-Encoding 且本端没有对应解码器：对端不按协商办事
        LocalResourceFailed,        ///< 本端资源或 zlib 状态问题（初始化失败、返回码非数据类错误）：与对端无关，可重试
    };

    /**
     * @brief 一次压缩正文解码失败：类别供分支，文案供人看
     *
     * @details 文案一律中文且写成「原因 + 下一步怎么做」，zlib 的原始返回码附在文案里作为证据。
     */
    struct ASYN_NET_API HttpBodyDecodeError
    {
        HttpBodyDecodeErrorKind kind;    ///< 失败类别，调用方据此选状态码与是否重试
        std::string             message; ///< 中文可操作文案，含编码名、上限值或 zlib 返回码
    };

    /**
     * @brief 压缩正文解码的结果形状：成功带 Value，失败侧固定为 HttpBodyDecodeError
     */
    template<typename Value>
    using HttpBodyDecodeExpected = std::expected<Value, HttpBodyDecodeError>;
} // namespace AsynGyanis::Net
