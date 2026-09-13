/**
 * @file WebSocketHandshake.h
 * @brief WebSocket（RFC 6455）握手：Accept 值计算、升级请求校验与 101 报文构建
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

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
     * @brief 计算握手应答里的 Sec-WebSocket-Accept 值
     *
     * @details 结果是 base64(SHA-1(clientKey + 固定 GUID))，GUID 为 "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
     *          （RFC 6455 §1.3）。SHA-1 走 OpenSSL 的 EVP 接口，不使用被标记为废弃的 SHA1() 便捷函数。
     * @param clientKey 客户端 Sec-WebSocket-Key 头部的值原文，按「指针 + 长度」取，可以是任意字节
     * @return std::string 24 个字符的 base64 文本，可直接写进 Sec-WebSocket-Accept
     * @throws Base::Exception OpenSSL 摘要接口不可用（库未正确初始化或内存不足）
     * @note 本函数不校验 clientKey 的形态：先用 isWebSocketUpgradeRequest() 判定，再用它算应答
     */
    [[nodiscard]] std::string computeWebSocketAcceptValue(std::string_view clientKey);

    /**
     * @brief 判定一条 HTTP 请求是否构成合法的 WebSocket 升级请求
     *
     * @details 逐条校验 RFC 6455 §4.1 对客户端的要求：GET 方法、HTTP 版本不低于 1.1、Upgrade 头含
     *          token "websocket"、Connection 头含 token "Upgrade"、Sec-WebSocket-Version 恰为 13、
     *          Sec-WebSocket-Key 是解码后恰 16 字节的标准 base64。任一条不满足即返回 false。
     * @param request 已解析完成的请求
     * @param failureReason 失败原因出参；进入调用时先清空，仅失败时写入中文原因，成功时保持为空
     * @return true 是合法的升级请求，可调用 buildHandshakeResponse() 回 101
     * @return false 不是升级请求，原因见 failureReason（传空指针则只要结论，不要原因）
     * @note token 比对大小写不敏感并按逗号拆分，因此 "Upgrade: WebSocket, foo" 这类写法同样被接受
     * @note 只做「是不是升级请求」的判定，不涉及鉴权：Origin、子协议与自定义头部留给上层
     */
    [[nodiscard]] bool isWebSocketUpgradeRequest(const HttpRequest &request, std::string *failureReason);

    /**
     * @brief 校验两种握手形态共用的两项：Sec-WebSocket-Version 恰为 13、Sec-WebSocket-Key 是解码后恰 16 字节的标准 base64
     *
     * @details h1 的升级握手（RFC 6455 §4.1）与 h2 的扩展 CONNECT 隧道（RFC 8441 §5）在这一点上完全一致，
     *          差别只在承载方式：前者靠 Upgrade/Connection 头，后者靠 :protocol=websocket。把这两项单独提出来，
     *          同一条规范要求就只有一个出处，两处不会各自漂移。
     * @param request 已解析完成的请求（h2 侧同样是已映射好的请求对象）
     * @param clientKey 输出参数：通过校验的 key 原文（已去掉首尾空白），可直接交给 computeWebSocketAcceptValue()；
     *        进入调用时先清空，仅成功时写入
     * @param failureReason 失败原因出参；进入调用时先清空，仅失败时写入中文原因
     * @return true 两项都通过，clientKey 可用
     * @return false 原因见 failureReason
     */
    [[nodiscard]] bool validateWebSocketKeyAndVersion(const HttpRequest &request, std::string &clientKey,
                                                      std::string *failureReason);

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
    [[nodiscard]] std::string buildHandshakeResponse(std::string_view clientKey, std::string_view extensionsResponseValue = {});
} // namespace AsynGyanis::Net
