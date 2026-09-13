/**
 * @file HttpParserLimits.h
 * @brief 单条 HTTP 报文的解析上限：请求行 / 头部 / 正文 / 分块行各维度的可配置限额
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>

namespace AsynGyanis::Net
{
    /**
     * @brief HTTP 解析器的资源上限配置。
     *
     * @details 与 HttpServerLimits 分工不同：连接级限额管时间与请求条数（空闲 / 读写超时、单连接
     *          请求上限），本结构管单条报文的内存占用，两者互不覆盖；头部行整行上限由名与值两项
     *          推出（名 + 值 + ": " 与 CRLF），因此不另设字段。
     * @note 每一项取值 0 都表示**关闭该项保护**（即不设上限），与 HttpServerLimits 的 0 语义一致，
     *       不是「不允许任何长度」：例如 maximumHeaderCount 为 0 时头部条数不限、maximumBodySize
     *       为 0 时正文多长都收。
     * @warning 关掉长度类上限会让解析器持续缓冲一整行或一条正文，确实要放行超大报文时才这么做；
     *          至少保留 maximumBodySize 与 maximumHeaderCount，否则等于放弃 DoS 防护。
     * @note 解析器在构造时按值取走一份配置，没有运行期换配置的接口：解析按字节增量推进，限额若在
     *       报文收了一半时变紧，同一个字段前后两段会按不同尺子判定，出错位置不可预期。
     * @see HttpServerLimits, HttpParser, HttpServer::setParserLimits(), HttpsServer::setParserLimits()
     */
    struct HttpParserLimits
    {
        std::size_t maximumRequestLineLength{8ull * 1024 + 32 + 16}; ///< 请求行整行上限，单位字节；默认 = URI 8 KiB + 方法 32 B + 版本与分隔空格 16 B。0 表示不限；放宽 URI 时要同时放宽本项
        std::size_t maximumUriLength{8ull * 1024};                   ///< 请求目标（URI）上限，单位字节。0 表示不限
        std::size_t maximumHeaderFieldNameLength{256};               ///< 单个头部名上限，单位字节；标准头名最长不过数十字节，留足 x-amz- 一类私有前缀。0 表示不限
        std::size_t maximumHeaderFieldValueLength{8ull * 1024};      ///< 单个头部值上限，单位字节；与 URI 同档，覆盖超长 Cookie 的现实用量。0 表示不限
        std::size_t maximumHeaderCount{100};                         ///< 头部条数上限，单位条；trailer 头部同样计入。0 表示不限条数
        std::size_t maximumHeaderBlockLength{64ull * 1024};          ///< 头部块总长上限，单位字节，只算名与值的净字节（不含 ": " 与 CRLF）。0 表示不限
        std::size_t maximumBodySize{8ull * 1024 * 1024};             ///< 正文上限，单位字节；分块按解码后的字节数累计。0 表示不限
        std::size_t maximumChunkSizeLineLength{1024};                ///< 分块块大小行上限，单位字节（含块扩展，不含 CRLF）。0 表示不限
    };

} // namespace AsynGyanis::Net
