/**
 * @file WebSocketError.h
 * @brief WebSocket 客户端两段的失败类别：升级协商（握手与扩展谈定）与会话收发（读帧与应答）
 * @author Gyanis
 * @date 2026-10-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 分两段是因为调用方的处置面不同：升级没谈成要么改地址与配置、要么怀疑中间盒改了应答；
 *          会话期失败要么重连、要么调上限、要么把对端的关闭原因交给业务。两类并成一个枚举会让
 *          receive() 的用户看见一堆永远进不去的取值，因此各一套 kind，各自带结果别名。
 */

#pragma once

#include "AsynGyanisExport.h"

#include <expected>
#include <string>

namespace AsynGyanis::Net
{
    /**
     * @brief 升级没谈成的类别
     *
     * @details 新增类别一律追加在末尾：取值会进日志与指标，中间插入会让既有取值的含义整体位移。
     *          每一格都必须有真实产出点；分档依据是「调用方下一步做什么」，不是 RFC 的条款号。
     */
    enum class WebSocketUpgradeErrorKind
    {
        LocalRequestInvalid,        ///< 本端交不出一份合法升级请求：缺 hostName、handshakeTimeout 非正、本端自己的扩展要约不合规
        AddressUnresolved,          ///< 目标主机名解析不出可用地址：重试无用，先核对主机名或改填 IP 字面量
        DeadlineExpired,            ///< 握手时限到点（建连、TLS、等 101 三段之一）：给更大的 handshakeTimeout 或查对端是否在该路径应答
        TransportFailed,            ///< 通路层面的失败：TCP 连不上、升级请求写不出去、读应答时被复位或关掉
        TlsSetupRejected,           ///< 本端 TLS 设置被 OpenSSL 拒（ALPN 列表写法不合）：与对端无关，改本端写法
        TlsHandshakeFailed,         ///< TLS 握手没成：证书未过校验、协议或套件不合、对端中途收线
        PeerClosedBeforeUpgrade,    ///< 对端在给出 101 之前就收了线：它多半没把这个路径当作 WebSocket 端点
        MalformedHttpResponse,      ///< 升级应答不是合法 HTTP 报文（状态行或头部读不懂、越出声明上限）：中间盒或端口配错的典型形状
        NotSwitchingProtocols,      ///< 应答状态不是 101 Switching Protocols：地址、端口或 TLS 配置与预期不符
        UpgradeHeadersInvalid,      ///< 101 的 Upgrade/Connection 头缺必要 token（RFC 6455 §4.2.2 第 2、3 条）：升上去的不是 WebSocket，或切换被中间设施挡下
        AcceptMismatch,             ///< Sec-WebSocket-Accept 缺失、多条或与本端算出的值不一致：这条应答不是回给本次握手的，按被改或串话处理
        SubprotocolMismatch,        ///< 子协议应答不合：选定值不在本端提议名单里、空值、含 token 之外的字符或多条拼成列表（RFC 6455 §4.2.2、§4.3）
        ExtensionEchoInvalid,       ///< 应答回了本端没提议过的扩展（RFC 6455 §9.1）：谈定的不是本端要的能力
        DeflateNegotiationRejected, ///< permessage-deflate 谈不成：响应参数越出要约或违反 RFC 7692 §7.1 的客户端失败条件
    };

    /**
     * @brief 一次升级没谈成：类别供分支，文案供人看
     */
    struct ASYN_NET_API WebSocketUpgradeError
    {
        WebSocketUpgradeErrorKind kind;    ///< 失败类别，调用方据此决定改配置、重试还是把连接判死
        std::string               message; ///< 中文可操作文案，含实际取值、条款号与目标地址
    };

    /**
     * @brief 会话期收发失败的类别
     *
     * @details 与升级段同一套追加规则；这一格的产出点在 receive() 与 answerPeerClose() 上。
     */
    enum class WebSocketSessionErrorKind
    {
        SessionClosed,               ///< 通路已经关了（本端收口或已发过 Close）：这条会话不会再收到消息
        MessageLimitExceeded,        ///< 解码越出消息或帧长度上限：按 1009 收口（RFC 6455 §7.4.1），要放宽就调上限
        ProtocolFramingRejected,     ///< 帧本身违例（掩码、保留位、控制帧分片等）：按 1002 收口，对端不按协议发
        CompressedMessageUnusable,   ///< 压缩消息解不开或解出后越出上限（RFC 7692 §7.2.2）：连接已按协议错误收口
        PeerClosedWithoutCloseFrame, ///< 对端没留下 Close 帧就收了线（§7.1.1 的快速断开）：拿不到对端给的原因码
        ConnectionLostWhileReading,  ///< 读帧期间通路出错：连接已断、被对端复位，或本端把它关了
        TextPayloadNotUtf8,          ///< 文本消息不是合法 UTF-8：按 1007 收口
    };

    /**
     * @brief 一次会话期收发失败：类别供分支，文案供人看
     */
    struct ASYN_NET_API WebSocketSessionError
    {
        WebSocketSessionErrorKind kind;    ///< 失败类别，调用方据此区分「会话没了」与「对端违例」
        std::string               message; ///< 中文可操作文案，含收口用的关闭码与对应条款
    };

    /**
     * @brief 升级协商的结果形状：成功带 Value，失败侧固定为 WebSocketUpgradeError
     */
    template<typename Value>
    using WebSocketUpgradeExpected = std::expected<Value, WebSocketUpgradeError>;

    using WebSocketUpgradeStatus = std::expected<void, WebSocketUpgradeError>; ///< 只问谈没谈成的协商结果

    /**
     * @brief 会话收发的结果形状：成功带 Value，失败侧固定为 WebSocketSessionError
     */
    template<typename Value>
    using WebSocketSessionExpected = std::expected<Value, WebSocketSessionError>;
} // namespace AsynGyanis::Net
