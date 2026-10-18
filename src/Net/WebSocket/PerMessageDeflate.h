/**
 * @file PerMessageDeflate.h
 * @brief permessage-deflate（RFC 7692）的消息级压缩与扩展协商
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"
#include "Net/WebSocket/WebSocketError.h"

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    /// 本端压缩/解压窗口位数的缺省档：zlib 的裸 deflate 最大窗口（32 KiB），也是 RFC 7692 允许的上界
    inline constexpr int kWebSocketDefaultWindowBits = 15;

    /// RFC 7692 §7.1.1/§7.1.2 承认的最小窗口位数：比这更小就不是合法的协商取值，而是对端写错了
    inline constexpr int kWebSocketMinimumWindowBits = 8;

    /**
     * @brief 一条连接上两侧各自的 deflate 窗口位数
     *
     * @details 这两个数不是本端的偏好，而是对端的声明：压缩窗口只要大过对端解压器的窗口，
     *          对方就会在远距离回溯上解不开（zlib 报 Z_DATA_ERROR），一条帧就把整条连接打死；
     *          解压窗口按对端声明的压缩位数收小，是按连接回收内存的那一位（15 档约 32 KiB、8 档 256 B）。
     */
    struct ASYN_NET_API PerMessageDeflateWindow
    {
        int compressBits{kWebSocketDefaultWindowBits};   ///< 本端压缩位数：不得超过对端 `server_max_window_bits` 声明的解压能力
        int decompressBits{kWebSocketDefaultWindowBits}; ///< 本端解压位数：按对端 `client_max_window_bits` 声明的压缩能力钳
    };

    /**
     * @brief permessage-deflate 的协商结论
     */
    struct ASYN_NET_API PerMessageDeflateNegotiation
    {
        bool                                   accepted{false}; ///< 是否接受该扩展
        std::string                            responseValue;   ///< 接受时回给对端的 Sec-WebSocket-Extensions 取值；拒绝时为空
        std::optional<PerMessageDeflateWindow> window{};        ///< 接受时的两侧窗口位数；未接受时为空——收发两侧按它决定压缩参数
    };

    /**
     * @brief 协商 permessage-deflate 扩展（RFC 7692 §7.1）
     *
     * @details 在客户端提供的 Sec-WebSocket-Extensions 里找 permessage-deflate；找到就接受并回一份本端
     *          选定的参数：`server_no_context_takeover` 与 `client_no_context_takeover`。两条都要求每条
     *          消息重置压缩上下文，因此收发都不需要跨消息保存 z_stream——代价是压缩率略低，换来连接级压缩
     *          状态及其生命周期管理的省却。
     *          两个窗口参数按 RFC 的语义吃掉而不是忽略：对端的 `server_max_window_bits` 是它解压器能开的
     *          最大窗口，本端压缩位数钳到它以下（写大了对端解不开，见 PerMessageDeflateWindow）；对端的
     *          `client_max_window_bits` 是它压缩时用的位数，本端解压窗口按它收小。
     *          婉拒的三种形态都出自 RFC 7692 §9.1 那张「服务器必须婉拒这条要约」的清单：取值不在 8..15
     *          或不是十进制数字、要约里出现了本扩展没定义的参数名、同名参数重复出现。前一种是无法履约，
     *          后两种是本端无从判断对端声明的到底是什么配置——带着猜出来的配置把连接开起来，或把不认识
     *          的要求当「与本端无关」放过，都是「看起来成功了」而线上未必。
     *          两条 `*_no_context_takeover` 是被识别的（本端本来就按每条消息重置上下文实现），但它们带值
     *          即取值不合法：那是布尔参数。
     * @param extensionsHeader Sec-WebSocket-Extensions 头部的值，缺头时传空串
     * @return PerMessageDeflateNegotiation 协商结论；未提供、提供了本端不认识的扩展名、或要约命中上面
     *         三种形态时不接受
     * @note 多个扩展可以逗号分隔并存（RFC 6455 §9.1），这里只挑出 permessage-deflate 那一个，
     *       其余扩展不参与协商、也不回进响应
     */
    [[nodiscard]] ASYN_NET_API PerMessageDeflateNegotiation negotiatePerMessageDeflate(std::string_view extensionsHeader);

    /**
     * @brief 压缩一条 WebSocket 消息（RFC 7692 §7.2.1）
     *
     * @details 步骤固定为：负载后追加四字节 `0x00 0x00 0xFF 0xFF`，用 裸 deflate（窗口位数取负值，
     *          不带 zlib 头尾）压到 Z_SYNC_FLUSH，再把输出末尾的四字节空块尾去掉——线上负载因此
     *          不含这四字节，解压侧自行补回。
     * @param payload 消息负载，可为空（空消息也会产出合法的压缩结果）
     * @param windowBits 压缩窗口位数，合法区间 8..15；必须不大于对端解压器声明的窗口，
     *        否则对端会在远距离回溯上报数据错误。越界取值返回空而不是悄悄按 15 压
     * @return std::optional<std::string> 线上负载；zlib 失败（内存不足或输出未按预期收尾）或窗口位数越界时为空，
     *         调用方应放弃压缩并原样发送（README 同 HTTP 侧的「绝不发坏字节」口径）
     * @note 本函数不判「压完是否更短」：空字典下短消息必然膨胀，而换不换表示是调用方的决定
     *       （RFC 7692 §7.3 把这条判据交给禁用了上下文接管的一端）
     */
    [[nodiscard]] ASYN_NET_API std::optional<std::string> deflateWebSocketMessage(std::string_view payload, int windowBits = kWebSocketDefaultWindowBits);

    /**
     * @brief 解压一条 WebSocket 消息
     *
     * @details 与 deflateWebSocketMessage() 互为逆运算：先补回四字节空块尾再裸 inflate。
     * @param payload 线上负载（对端发来的压缩字节）
     * @param maximumOutputBytes 解压输出的字节上限，0 表示不限
     * @param windowBits 解压窗口位数，合法区间 8..15；按对端声明的压缩位数（协商结果）给定，
     *        越界取值返回空；比实际需要开得更大不会解不开，但每连接多占一份窗口内存
     * @return std::optional<std::string> 原始消息；数据非法、窗口位数越界或解压结果超过上限时为空——
     *         上限必须由调用方给出：压缩比可以做到几百倍，不设上限时一条小消息就能把内存撑爆（zip bomb）
     */
    [[nodiscard]] ASYN_NET_API std::optional<std::string> inflateWebSocketMessage(std::string_view payload, std::size_t maximumOutputBytes,
                                                                                  int windowBits = kWebSocketDefaultWindowBits);

    /// 每条消息的默认压缩级别：与 HTTP 响应压缩取同一档（zlib 的 6）
    inline constexpr int kWebSocketDeflateLevel = 6;

    /// 扩展所在的头部名（RFC 7692 §7.1）：h1 的请求/响应头与 h2 的头字段名都是它，
    /// 因此协商的读入口与写出口共用同一个出处
    inline constexpr std::string_view kWebSocketExtensionsHeaderName = "sec-websocket-extensions";

    /// 本端在扩展要约里唯一提议、也唯一会接受的那个扩展名（RFC 7692 §7.1）：
    /// 要约与回显两侧都按它比对，不再在别处写第二份字面量
    inline constexpr std::string_view kPerMessageDeflateExtensionName = "permessage-deflate";

    /**
     * @brief 客户端要约的线上形态（RFC 7692 §7.1.1/§7.1.2）
     *
     * @details 四段各有出处：`permessage-deflate` 是扩展名；`server_no_context_takeover` 是本端
     *          对服务端的硬要求（本端解压器无状态、不保留跨消息的 z_stream，服务端不承诺每条
     *          消息重置上下文时本端无法安全解压，见 acceptPerMessageDeflateResponse 的拒因）；
     *          `client_no_context_takeover` 是对称的承诺类 hint（本端发送本就不复用上下文）；
     *          `client_max_window_bits` 不带值＝声明本端接受服务端在响应里指定客户端的压缩窗口
     *          （§7.1.2.2 的无值形态）。刻意不带 `server_max_window_bits`：那会向服务端提出
     *          「你压缩窗口不得大于 N」的限定，而服务端即使要约没带也可在响应里主动声明自己的
     *          压缩窗口（§7.1.2.1 的 MAY），本端按声明值解压即可。
     */
    inline constexpr std::string_view kPerMessageDeflateClientOffer = "permessage-deflate; server_no_context_takeover; client_no_context_takeover; client_max_window_bits";

    /**
     * @brief 客户端接受服务端对 permessage-deflate 的响应结论（RFC 7692 §7.1 的客户端判据）
     *
     * @details 逐条落实 §7.1 列出的四类「客户端必须让连接失败」的响应形态：含本扩展未定义的
     *          参数名、含取值非法的参数（no_context_takeover 两条带值、窗口位数不是 8..15 的
     *          无前导零十进制）、同名参数重复出现、以及响应代表的配置本端支持不了。后者在本实现
     *          里具体化为两条：①要约带了 `server_no_context_takeover` 而响应没带——服务端没承诺
     *          每条消息重置上下文，本端解压器（无状态）接不住这种配置；②响应带
     *          `client_max_window_bits` 而要约为没带——§7.1.2.2 明写这种响应 MUST NOT 出现，
     *          对上了说明对面实现不对。窗口取值的两处方向各按 §7.1.2 归位：本端解压窗口按响应的
     *          `server_max_window_bits`（服务端按它为压缩窗口）、本端压缩窗口按响应的
     *          `client_max_window_bits`（服务端限制本端压缩窗口），缺省都是 15。服务端即使要约
     *          没带 `server_max_window_bits` 也可在响应里主动带它（§7.1.2.1 的 MAY），因此这条
     *          不要求要约先带过。
     * @param responseHeaderValue 101 应答里 Sec-WebSocket-Extensions 的全部取值拼成的一段文本
     * @param offerHeaderValue 本端发出去的同名头部原文（含参数；用于校验响应与要约的一致性）
     * @return WebSocketUpgradeExpected<std::optional<PerMessageDeflateWindow>>
     *         响应里没有 permessage-deflate 时交回空 optional（未启用扩展，连接按明文继续）；
     *         无法接受时交回中文原因（点明判不过的是哪一条）；可接受时交回两侧窗口位数
     * @note 响应里出现两段 permessage-deflate 元素按「同名重复」拒绝：哪一段是服务端真正的意思
     *       无从判断，带着猜出来的配置开连接是「看起来成功了」的最坏形态
     */
    [[nodiscard]] ASYN_NET_API WebSocketUpgradeExpected<std::optional<PerMessageDeflateWindow>> acceptPerMessageDeflateResponse(std::string_view responseHeaderValue,
                                                                                                                                std::string_view offerHeaderValue);
} // namespace AsynGyanis::Net
