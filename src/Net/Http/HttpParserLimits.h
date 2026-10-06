/**
 * @file HttpParserLimits.h
 * @brief 单条 HTTP 报文的解析上限：请求行 / 头部 / 正文 / 分块行各维度的可配置限额
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Net/WebSocket/WebSocketFrame.h"

#include <cstddef>

namespace AsynGyanis::Net
{
    /// 请求行里除 URI 之外的固定余量，单位字节。判的是**行体**（方法 SP 目标 SP 版本），行尾那两字节
    /// 不参与计数——与 `maximumChunkSizeLineLength` 那句「含块扩展，不含 CRLF」同一口径，见
    /// `HttpHeaderRules.h` 的 lineBodyByteCountWithTerminator。拆法：方法名上限 32 B
    /// （HttpParser::kMaximumMethodLength，与 llhttp 同档的协议语法约束）+ 版本串与分隔符 10 B
    /// （"HTTP/1.1" 8 B + 两个分隔空格 2 B）+ 6 B 余量（版本号位数，以及行尾 CRLF 落在下一段时的容错）。
    /// 整行上限由它加上 maximumUriLength 推出，故本结构没有请求行长度的独立字段。
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
    struct ASYN_NET_API HttpParserLimits
    {
        std::size_t maximumUriLength{8ull * 1024};              ///< 请求目标（URI）上限，单位字节；请求行整行上限也由它推出（见 requestLineLengthLimit()）。0 表示不限
        std::size_t maximumHeaderFieldNameLength{256};          ///< 单个头部名上限，单位字节；标准头名最长不过数十字节，留足 x-amz- 一类私有前缀。0 表示不限
        std::size_t maximumHeaderFieldValueLength{8ull * 1024}; ///< 单个头部值上限，单位字节；与 URI 同档，覆盖超长 Cookie 的现实用量。0 表示不限
        std::size_t maximumHeaderCount{100};                    ///< 头部条数上限，单位条；trailer 头部同样计入。0 表示不限条数
        /// 头部块总长上限，单位字节，只算名与值的净字节（不含 ": " 与 CRLF）。0 表示不限。
        /// 三条通道都判这一把尺：h1 由解析器判，h3 在会话层判，h2 在 intake 判，且与 maximumHeaderCount
        /// 同样是**整条报文累计**（h2 把尾部头块的净字节加在头部那一场之上）。明写的例外：h2/h3 的
        /// 流式路由在头收齐那刻就把请求派发了，此后越限只拦得住尾字段、回不出 431，那一段体量由各自的
        /// 压缩层上限（HPACK 逐条尺 / SETTINGS_MAX_FIELD_SECTION_SIZE）兜住。h2 另有
        /// `Http2ConnectionConfiguration::maximumHeaderListSize`（按 RFC 9113 §6.5.2 的「名长 + 值长 + 32」
        /// 逐条计，且随 SETTINGS 宣告给对端）并排列着，两者取更紧的一方生效；出厂值同为 64 KiB，
        /// 要在 h2 那一侧再放宽走 `HttpServer::setHttp2Configuration()`。
        /// 本结构里除 maximumChunkSizeLineLength（分块只存在于 h1，h2/h3 禁掉 Transfer-Encoding 就没有
        /// 块大小行可判）之外每一项都在**三条通道**上生效（h1 由解析器判，h2 与 h3 在 intake 判；
        /// 两条长度项在 h2 由 HPACK 解码器按同一把尺判，见 Http2Connection 的构造），
        /// 配置键因此只有一个含义
        std::size_t maximumHeaderBlockLength{64ull * 1024};
        std::size_t maximumBodySize{8ull * 1024 * 1024}; ///< 正文上限，单位字节；分块按解码后的字节数累计。0 表示不限
        std::size_t maximumChunkSizeLineLength{1024};    ///< 分块块大小行上限，单位字节（含块扩展，不含 CRLF）。0 表示不限
        /// WebSocket 隧道里一条入站消息的字节上限，单位字节：分片重组后与 permessage-deflate 解压后都按这一把尺
        /// 判（单帧上限也是它——一条消息拆不拆片都不得超过）。0 表示不限，与其余各项同一口径。
        /// 三条通道都吃到这一格：h1 的升级、h2 的升级与 h3 的 CONNECT 隧道各自在建 `WebSocketPeer` 时把本值
        /// 交下去，因此默认值直接取解码层那个类常量（数值只在 `WebSocketFrameDecoder` 里写一次）。
        /// 新字段一律加在末尾：本结构是聚合体，往中间插会让按位置初始化的调用方（含仓库外的使用者）静默错位
        std::size_t maximumWebsocketMessageSize{WebSocketFrameDecoder::kMaximumMessagePayloadLength};

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
