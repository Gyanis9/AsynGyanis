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
    /// 请求行（方法 SP 目标 SP 版本 CRLF）里除 URI 之外的固定余量，单位字节：方法名上限 32 B
    /// （HttpParser::kMaximumMethodLength，与 llhttp 同档的协议语法约束）+ 版本串与分隔符 16 B
    /// （"HTTP/1.1" 8 B + 两个分隔空格 2 B + 行尾 CRLF 2 B，另余 4 B 给版本号位数）。整行上限由它
    /// 加上 maximumUriLength 推出，故本结构没有请求行长度的独立字段。
    inline constexpr std::size_t kRequestLineFixedOverheadBytes = 32 + 16;

    /**
     * @brief HTTP 解析器的资源上限配置。
     *
     * @details 与 HttpServerLimits 分工不同：连接级限额管时间与请求条数（空闲 / 读写超时、单连接
     *          请求上限），本结构管单条报文的内存占用，两者互不覆盖；头部行与请求行的整行上限都由
     *          已有字段推出（名 + 值 + ": " 与 CRLF、URI + 方法名与版本串的固定余量），不另设字段。
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
        std::size_t maximumUriLength{8ull * 1024};              ///< 请求目标（URI）上限，单位字节；请求行整行上限也由它推出（见 requestLineLengthLimit()）。0 表示不限
        std::size_t maximumHeaderFieldNameLength{256};          ///< 单个头部名上限，单位字节；标准头名最长不过数十字节，留足 x-amz- 一类私有前缀。0 表示不限
        std::size_t maximumHeaderFieldValueLength{8ull * 1024}; ///< 单个头部值上限，单位字节；与 URI 同档，覆盖超长 Cookie 的现实用量。0 表示不限
        std::size_t maximumHeaderCount{100};                    ///< 头部条数上限，单位条；trailer 头部同样计入。0 表示不限条数
        std::size_t maximumHeaderBlockLength{64ull * 1024};     ///< 头部块总长上限，单位字节，只算名与值的净字节（不含 ": " 与 CRLF）。0 表示不限
        std::size_t maximumBodySize{8ull * 1024 * 1024};        ///< 正文上限，单位字节；分块按解码后的字节数累计。0 表示不限
        std::size_t maximumChunkSizeLineLength{1024};           ///< 分块块大小行上限，单位字节（含块扩展，不含 CRLF）。0 表示不限

        /**
         * @brief 推导请求行整行的长度上限
         * @details 请求行 = 方法 SP 目标 SP 版本 CRLF，除目标之外的固定部分由
         *          kRequestLineFixedOverheadBytes 盖住；整行上限因此完全跟着 maximumUriLength 走。
         *          目标上限为 0（不限）时整行同样不限。
         * @return std::size_t 整行上限，单位字节：maximumUriLength + 固定余量；0 表示不设上限
         */
        [[nodiscard]] constexpr std::size_t requestLineLengthLimit() const noexcept
        {
            // 0 表示关闭该项保护：URI 不限时整行也不能只剩那几十字节余量，而是同样不设上限
            return maximumUriLength == 0 ? std::size_t{0} : maximumUriLength + kRequestLineFixedOverheadBytes;
        }
    };

} // namespace AsynGyanis::Net
