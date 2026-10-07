/**
 * @file WebSocketHandshake.h
 * @brief WebSocket（RFC 6455）握手：Accept 值计算、升级请求校验与 101 报文构建
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Base/Exception/Exception.h"
#include "Net/Http/HttpRequest.h"

#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    // ============================================================================
    // WebSocket 握手（RFC 6455 §4）：三个纯计算的自由函数
    //
    // 尚未接入会话：把升级请求认出来、回完 101 之后怎么收发帧都还没做，本组接口只提供
    // 协议机制。因此引入本文件不会改变任何既有 HTTP 会话的行为。
    // ============================================================================

    /**
     * @brief 握手失败的分类
     *
     * @details 只有一类失败带着一份额外的应答义务：版本不被理解时，应答**必须**带
     *          Sec-WebSocket-Version 指明本端支持的版本（RFC 6455 §4.2.2），客户端正是靠这一行
     *          决定换一个版本重试。其余缺陷（缺 key、base64 形态不对、不是 GET）没有这条义务，
     *          带上反而会把一个「你发的 key 不对」的应答伪装成「版本不对」。分类由校验函数
     *          就地给出，判据只有一个出处，三个协议的会话不许各自再判一遍。
     */
    enum class WebSocketHandshakeRejection
    {
        None,               ///< 校验通过
        UnsupportedVersion, ///< 版本缺失或不是本端支持的那一档：应答要带 Sec-WebSocket-Version
        Other,              ///< 其余握手缺陷：没有那条头部义务
    };

    /// 本实现支持的 WebSocket 协议版本（RFC 6455）：校验判的那个数与拒绝应答里回的那个数为同一出处
    inline constexpr std::string_view kSupportedWebSocketVersion = "13";

    /// 拒绝版本不合的握手时必须带的那条头部名（RFC 6455 §4.2.2）
    inline constexpr std::string_view kWebSocketVersionHeaderName = "sec-websocket-version";

    /**
     * @brief 计算握手应答里的 Sec-WebSocket-Accept 值
     *
     * @details 结果是 base64(SHA-1(clientKey + 固定 GUID))，GUID 为 "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
     *          （RFC 6455 §1.3）。SHA-1 走 OpenSSL 的 EVP 接口，不使用被标记为废弃的 SHA1() 便捷函数。
     * @param clientKey 客户端 Sec-WebSocket-Key 头部的值原文，按「指针 + 长度」取，可以是任意字节
     * @return std::string 24 个字符的 base64 文本，可直接写进 Sec-WebSocket-Accept
     * @throws Base::Exception OpenSSL 摘要接口不可用（库未正确初始化或内存不足）
     * @note 本函数不校验 clientKey 的形态：先用 isWebSocketUpgradeRequest() 判定，再用它算应答
     */
    [[nodiscard]] ASYN_NET_API std::string computeWebSocketAcceptValue(std::string_view clientKey);

    /**
     * @brief 判定一条 HTTP 请求是否构成合法的 WebSocket 升级请求
     *
     * @details 逐条校验 RFC 6455 §4.1 对客户端的要求：GET 方法、HTTP 版本不低于 1.1、Upgrade 头含
     *          token "websocket"、Connection 头含 token "Upgrade"、Sec-WebSocket-Version 恰为 13、
     *          Sec-WebSocket-Key 是解码后恰 16 字节的标准 base64。任一条不满足即返回 false。
     * @param request 已解析完成的请求
     * @param failureReason 失败原因出参；进入调用时先清空，仅失败时写入中文原因，成功时保持为空
     * @param rejection 失败分类出参，可传空指针：版本类失败要据此在拒绝应答里补一条 Sec-WebSocket-Version
     * @return true 是合法的升级请求，可调用 buildHandshakeResponse() 回 101
     * @return false 不是升级请求，原因见 failureReason（传空指针则只要结论，不要原因）
     * @note token 比对大小写不敏感并按逗号拆分，因此 "Upgrade: WebSocket, foo" 这类写法同样被接受
     * @note 只做「是不是升级请求」的判定，不涉及鉴权：Origin、子协议与自定义头部留给上层
     */
    [[nodiscard]] ASYN_NET_API bool isWebSocketUpgradeRequest(const HttpRequest &request, std::string *failureReason, WebSocketHandshakeRejection *rejection = nullptr);

    /**
     * @brief 校验 h1 升级握手共用的两项：Sec-WebSocket-Version 恰为 13、Sec-WebSocket-Key 是解码后恰 16 字节的标准 base64
     *
     * @details 这一条服务的是 RFC 6455 §4.1 的 101 握手：那条路上 key 与版本都是客户端必填项，少一项就不构成
     *          一次合法握手。扩展 CONNECT 隧道（h2/h3）**不用**本函数判——见 validateWebSocketTunnelVersion()
     *          里引的 RFC 8441 §5。两条入口共用的那段版本判据住在同一个 .cpp 里的一份实现上，判据不会各写一遍。
     * @param request 已解析完成的请求（h2/h3 侧同样是已映射好的请求对象）
     * @param clientKey 输出参数：通过校验的 key 原文（已去掉首尾空白），可直接交给 computeWebSocketAcceptValue()；
     *        进入调用时先清空，仅成功时写入
     * @param failureReason 失败原因出参；进入调用时先清空，仅失败时写入中文原因
     * @param rejection 失败分类出参，可传空指针：版本类失败要据此在拒绝应答里补一条 Sec-WebSocket-Version
     * @return true 两项都通过，clientKey 可用
     * @return false 原因见 failureReason
     */
    [[nodiscard]] ASYN_NET_API bool validateWebSocketKeyAndVersion(const HttpRequest &request, std::string &clientKey, std::string *failureReason,
                                                                   WebSocketHandshakeRejection *rejection = nullptr);

    /**
     * @brief 校验扩展 CONNECT 隧道（RFC 8441 §5 / RFC 9220 §3）的握手：只判 Sec-WebSocket-Version
     *
     * @details 隧道形态不重做 101 那套 key/accept 处理——RFC 8441 §5 原文：「Implementations using this
     *          extended CONNECT to bootstrap WebSockets do not do the processing of the Sec-WebSocket-Key
     *          and Sec-WebSocket-Accept header fields of [RFC6455] as that functionality has been superseded
     *          by the :protocol pseudo-header field」。所以这里既不要求 key，也不校验它的形状。
     *          版本这一项按同一条 RFC 沿用 RFC 6455：**写出来却不是 13 必须拒**（并按 RFC 6455 §4.2.2 在拒绝
     *          应答里补一条 Sec-WebSocket-Version 指明本端支持的版本）；**整条没写不在这里拒**——那是 RFC 6455
     *          给 101 客户端派的义务，而按隧道形态实现的互操作端有两条都不带的（本仓的 aioquic 验收裁判就是），
     *          在这里拒等于把一条能用的隧道判死。本仓上一版在 h3 上加过 key 必填，被该裁判当场抓出。
     * @param request 已映射好的请求对象（h2/h3 侧都是同一份 HttpRequest）
     * @param failureReason 失败原因出参；进入调用时先清空，仅失败时写入中文原因
     * @param rejection 失败分类出参，可传空指针
     * @return true 可以建隧道（版本缺失，或恰为本端支持的那一档）
     * @return false 版本写了但不合，原因见 failureReason
     */
    [[nodiscard]] ASYN_NET_API bool validateWebSocketTunnelVersion(const HttpRequest &request, std::string *failureReason, WebSocketHandshakeRejection *rejection = nullptr);

    /**
     * @brief 扩展 CONNECT 的 :protocol 取值本端认不认（RFC 8441 §4 / RFC 9220 §3）
     * @details 三档而不是两面旗：除了「普通请求」与「websocket 隧道」，还有一整类是对端要跑一个
     *          本端没实现的协议，它既不是普通请求也不能当隧道——按 501 收口才说得清「协议没实现」，
     *          交给路由只会回 404/405。判定住在这一处，h2 与 h3 不再各比各的字符串字面量。
     */
    enum class ExtendedConnectKind
    {
        NotExtended, ///< :protocol 缺席：普通请求
        WebSocket,   ///< :protocol=websocket：应答 200 后这条流成为隧道
        Unsupported, ///< :protocol 是别的协议名：本端没实现，回 501
    };

    /// 扩展 CONNECT 唯一支持的协议名（:protocol 的取值，按 RFC 8441 的 token 逐字比较）
    inline constexpr std::string_view kWebSocketProtocolName = "websocket";

    /**
     * @brief 把 :protocol 原文分成上面三档
     * @param protocolValue :protocol 伪头原文，缺席时传空
     * @return ExtendedConnectKind 这条请求属于哪一档
     */
    [[nodiscard]] constexpr ExtendedConnectKind classifyExtendedConnect(const std::string_view protocolValue) noexcept
    {
        if (protocolValue.empty())
        {
            return ExtendedConnectKind::NotExtended;
        }
        return protocolValue == kWebSocketProtocolName ? ExtendedConnectKind::WebSocket : ExtendedConnectKind::Unsupported;
    }

    /**
     * @brief 构建 101 Switching Protocols 的完整应答报文
     *
     * @details 报文依次是状态行、Upgrade、Connection、Sec-WebSocket-Accept 与结束空行，
     *          行分隔符一律 CRLF，可直接整块写入连接。
     * @param clientKey 已通过 isWebSocketUpgradeRequest() 校验的 Sec-WebSocket-Key 值
     * @param extensionsResponseValue 回给对端的 Sec-WebSocket-Extensions 取值（见
     *        negotiatePerMessageDeflate()）；为空表示不启用任何扩展，此时不写该头部
     * @return std::string 完整的 101 报文，逐字节为
     *         "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
     *         "Sec-WebSocket-Accept: <值>\r\n\r\n"
     * @note 本实现不协商任何子协议：报文里不含 Sec-WebSocket-Protocol。扩展只在
     *       extensionsResponseValue 非空时写一行 Sec-WebSocket-Extensions
     */
    [[nodiscard]] ASYN_NET_API std::string buildHandshakeResponse(std::string_view clientKey, std::string_view extensionsResponseValue = {});
} // namespace AsynGyanis::Net
