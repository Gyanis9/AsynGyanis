/**
 * @file ProxyProtocol.h
 * @brief PROXY 协议（v1 文本行与 v2 二进制块）的读侧解析：把负载均衡器交来的原始四层身份拆成地址对
 * @author Gyanis
 * @date 2026-09-26
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once

#include "AsynGyanisExport.h"

#include "Core/Socket/InetAddress.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief v1（文本行）头的总长度上限
     * @details HAProxy 规定一条 v1 头不超过 107 字节 + CRLF，取 108 作硬上界：读侧没有这个上界，
     *          一个只发 `PROXY ` 然后一字节一挤的对端就能把接收缓冲撑住
     */
    inline constexpr std::size_t kMaximumProxyHeaderV1Bytes = 108U;

    /**
     * @brief v2（二进制块）头的总长度上限（含 16 字节定长段与 TLV）
     * @details v2 的长度字段是 16 位，理论上允许 64 KiB−16；实际地址块最多 36 字节（IPv6 两端 +
     *          两个端口），其余都是 TLV。1 KiB 足够装下今天所有已定义的 TLV，又让恶意对端没法用
     *          一个谎报的长度字段把服务器拖进大缓冲
     */
    inline constexpr std::size_t kMaximumProxyHeaderV2Bytes = 1024U;

    /**
     * @brief v2 TLV 里几个已定义的类型号（RFC 的 PROXY 协议规范附录 A）
     * @details 只列有名字的那几个；私有号段（0xE0–0xFF）与未定义类型一律按「原样交出、消费方按类型挑
     *          自己认的」处理，因此不在此处穷举。`NoOp` 是**填充**（规范要求接收方忽略它的内容），
     *          本层照原样放进列表——过滤是消费方的事，解析层不做取舍
     */
    enum class ProxyTlvType : std::uint8_t
    {
        Alpn      = 0x01, ///< PP2_TYPE_ALPN：上层协议协商结果
        Authority = 0x02, ///< PP2_TYPE_AUTHORITY：客户端请求的原始主机名（UTF-8）
        Crc32C    = 0x03, ///< PP2_TYPE_CRC32C：前面整条头的校验和（本层不校验，只交出）
        NoOp      = 0x04, ///< PP2_TYPE_NOOP：填充，内容无意义
        UniqueId  = 0x05, ///< PP2_TYPE_UNIQUE_ID：代理侧的一次性连接标识
        Ssl       = 0x20  ///< PP2_TYPE_SSL：客户端 TLS 信息（其值本身是一个嵌套的 TLV 序列）
    };

    /**
     * @brief v2 头里的一条 TLV：类型 + 值
     *
     * @note `value` 是**视图**，指进 `parseProxyHeader()` 收到的那个缓冲：解析结果不得活得比它长。
     *       这样定形是为了让解析入口保持不分配（它是 `noexcept` 的读侧热路径，每条连接都要走一次），
     *       调用方真要长期持有某条值，自己拷一份（单条值的上限就是整条头的
     *       kMaximumProxyHeaderV2Bytes）。
     */
    struct ASYN_NET_API ProxyTlv
    {
        std::uint8_t     type{0}; ///< 类型号，已知取值见 ProxyTlvType
        std::string_view value{}; ///< 值，按字节原样（二进制安全）
    };

    /**
     * @brief 一条 PROXY 头交出的四层身份
     *
     * @note **TOA 不做（这是决定，不是待办）**：TOA（TCP Option Address）是 Linux 专有的方案，要内核
     *       打补丁、在 TCP 选项里塞入真实来源，而且只有特定 LVS 形态会配它。「拿到真实来源」这一需求
     *       已由 PROXY 协议覆盖（本文件就是它的读侧），而 TOA 还要在本层之外额外接一条 `getsockopt`
     *       式的读法（对本项目来说既没有可测的内核，也没有可复现的部署形态）。真遇到只认 TOA 的
     *       LVS 时，落点是「在 accept 之后从 TCP 选项里读出地址再喂给本层的同一条身份出口」，
     *       而不是改解析器。
     */
    struct ASYN_NET_API ProxyEndpoint
    {
        Core::InetAddress source{};            ///< 真实客户端地址（协议里写的「源」）
        Core::InetAddress destination{};       ///< 被代理的本端地址（协议里写的「目的」）
        bool              hasAddresses{false}; ///< 这条头是否带地址：v1 的 `UNKNOWN` 与 v2 的 LOCAL 都不带

        /// v2 的 TLV，按出现顺序逐条交出（含填充用的 NoOp 与未定义类型，原样不取舍）；v1 恒为空。
        /// 值的视图指进解析时传入的那个缓冲，见 ProxyTlv 的说明
        std::vector<ProxyTlv> tlvs{};
    };

    /**
     * @brief 已读到的前缀能给读侧的答复：还需不需要继续读、这条连接是不是已经判死
     */
    struct ASYN_NET_API ProxyHeaderFraming
    {
        bool                       isStillPlausible{true}; ///< 前缀仍可能是一条头；为 false 时读侧应立刻判死，别再等
        std::optional<std::size_t> totalLength{};          ///< 已能定出整条头的字节数；还没定出时为空
    };

    /**
     * @brief 根据已读到的前缀判断这条头总共多少字节、还读不读得下去
     * @param buffered 已读到的字节（可以是半条头）
     * @return ProxyHeaderFraming 见结构体两个字段的说明
     * @details v1 靠 `PROXY ` 前缀认出、整条以第一个 CRLF 收尾（长度受 kMaximumProxyHeaderV1Bytes
     *          约束）；v2 靠 12 字节签名认出、整条长度 = 16 + 长度字段（受
     *          kMaximumProxyHeaderV2Bytes 约束）。签名对不齐的两种情形：前缀太短还判不出（继续读）、
     *          已经能断定不是头（例如以 `GET /` 开头——这是直接把服务器暴露在了公网上的形状）。
     */
    [[nodiscard]] ASYN_NET_API ProxyHeaderFraming frameProxyHeader(std::string_view buffered) noexcept;

    /**
     * @brief 解析一条**完整**的 PROXY 头
     * @param header 头的全部字节（可以比头本身长：只解析最前面那一条，多余部分归调用方的下一段读）
     * @param consumedBytes 输出参数：本次吃掉的字节数（v1 含结尾 CRLF，v2 含 TLV）；**解析失败时置 0**，
     *        调用方因此不必分辨「没动」与「没吃」——失败就是什么都没吃
     * @return std::optional<ProxyEndpoint> 解析成功交出身份；格式不合规范交出空
     * @note 不合规就返回空而不是抛：读侧的处置是「关掉这条连接并记一条日志」，一条报文格式问题不该
     *       把异常穿过接受循环
     * @note v1 的 `UNKNOWN` 与 v2 的 `LOCAL` 都会交出 `hasAddresses=false`：这两类头只说明
     *       「前面确实有个代理」，没有可当作客户端的身份。把它们当地址用等于用一个不存在的来源记账
     */
    [[nodiscard]] ASYN_NET_API std::optional<ProxyEndpoint> parseProxyHeader(std::string_view header, std::size_t &consumedBytes) noexcept;
} // namespace AsynGyanis::Net
