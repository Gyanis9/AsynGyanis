/**
 * @file WebSocketUpgradePolicy.h
 * @brief WebSocket 升级握手策略：Origin 准入与子协议协商（RFC 6455 §4.2.2 / §10.2、RFC 6454）
 * @author Gyanis
 * @date 2026-10-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief Origin 准入的三种形态
     *
     * @details 默认档是 SameOriginOnly：本仓是服务端框架，浏览器脚本是唯一能伪造 Origin 之外的来源身份
     *          的一方，而 RFC 6455 §10.2 对「不是给任意网页用的」服务端的措辞是 SHOULD verify。
     *          业界三家（Spring `setAllowedOrigins`、SignalR `WebSocketOptions.AllowedOrigins`、
     *          Django Channels `AllowedOriginRegex`）也都把「只放同源」当出厂默认，要放开得显式写。
     */
    enum class WebSocketOriginMode
    {
        SameOriginOnly, ///< 只放同源（默认）：Origin 的协议+主机+端口必须与本连接的目标一致
        AllowList,      ///< 放同源之外，再放行 allowedOrigins 里逐个列出的源
        AllowAny,       ///< 不校验 Origin：给「任何网页都能连」的公开端点用，必须显式选这一档
    };

    /**
     * @brief 谈不成子协议时的处置
     *
     * @details 默认 ProceedWithoutEcho 是 RFC 6455 §4.2.2 的字面口径：「客户端请求了而服务端一个都不
     *          接受时，唯一可发的值是 null」，也就是不回显、握手照常完成，由客户端按同节步骤 6 自己
     *          判失败。选 RejectHandshake 的场合是「本服务在不同子协议上语义不同，降级应答比拒更危险」——
     *          那要调用方显式声明，因为拒掉的是 RFC 允许成立的握手。
     */
    enum class WebSocketSubprotocolMismatch
    {
        ProceedWithoutEcho, ///< 不回显子协议、握手照常完成（默认，规范口径）
        RejectHandshake,    ///< 谈不成就按 400 拒掉这次升级
    };

    /**
     * @brief 一次 WebSocket 升级要按哪些规则判
     *
     * @details 由业务在 `HttpResponse::upgradeToWebSocket()` 上登记，三条入站通道（h1 的 `Upgrade`、
     *          h2 的扩展 CONNECT、h3 的 CONNECT 隧道）读同一份，判据不因承载方式而分叉。
     *          字段都有真实消费点：`isAllowedOrigin` 与 `selectSubprotocol` 各读一半，没有留着的空开关。
     */
    struct ASYN_NET_API WebSocketUpgradePolicy
    {
        /// Origin 准入形态
        WebSocketOriginMode originMode{WebSocketOriginMode::SameOriginOnly};

        /// AllowList 档下额外放行的源，写法是序列化后的源（`https://example.com`，不带路径）；比对前折小写
        std::vector<std::string> allowedOrigins{};

        /// 本端能讲的子协议，按对端给出的次序挑第一个本端支持的；空表表示不参与子协议协商
        std::vector<std::string> subprotocolCandidates{};

        /// 一条都没谈成时的处置，默认按规范不回显
        WebSocketSubprotocolMismatch subprotocolMismatch{WebSocketSubprotocolMismatch::ProceedWithoutEcho};
    };

    /**
     * @brief 判一条 Origin 在这份策略下能不能放行
     *
     * @details Origin 缺席按放行处理，不是漏判：RFC 6455 §4.1 第 8 条只要求**浏览器**客户端必发 Origin，
     *          非浏览器客户端「MAY include」——本仓自己的 `WebSocketClient` 与 curl 一类工具都不发，
     *          把它判死等于把自己家的客户端和整条 Autobahn 套件挡在门外。Origin 存在时的比较口径是
     *          「协议 + 主机 + 端口」三者全等（RFC 6454 §4 的同源定义），写法上的大小写差异与结尾的
     *          根点不参与判定。
     *
     * @param originValue 请求里 `Origin` 的原文；缺席时传空视图
     * @param sameOrigin 本连接自己的源（`scheme://host[:端口]`，scheme 取 `http`/`https`），由会话按
     *        请求的权威段与 `HttpRequest::overTls()` 拼出；空表示本连接无从判定同源
     * @param policy 生效的策略
     * @return true 可以放行这次升级
     * @return false 必须拒绝，拒绝理由见 describeWebSocketOriginRejection()
     */
    [[nodiscard]] ASYN_NET_API bool isWebSocketOriginAllowed(std::string_view originValue, std::string_view sameOrigin, const WebSocketUpgradePolicy &policy) noexcept;

    /**
     * @brief 把一次 Origin 拒绝写成给业务与日志看的中文原因
     * @details 与判定分开是给会话用的：会话要么只要一个布尔、要么在拒绝分支里拿文案回 403 并记日志，
     *          两段判据必须同解，所以文案与判定吃同一份归一化。
     * @param originValue 请求里 `Origin` 的原文
     * @param sameOrigin 本连接自己的源
     * @param policy 生效的策略
     * @return std::string 中文原因，写明「哪一格不符 + 该怎么放开」；放行时返回空串
     */
    [[nodiscard]] ASYN_NET_API std::string describeWebSocketOriginRejection(std::string_view originValue, std::string_view sameOrigin, const WebSocketUpgradePolicy &policy);

    /**
     * @brief 拼出本连接自己的源文本，喂给 isWebSocketOriginAllowed() 的同源那一格
     *
     * @details 协议名不由请求决定而由传输层决定：h1 走 `--https` 与 h3 一律是 TLS，同一条权威段在
     *          两种传输上折出的源不同（端口也不同），拿请求里的 `:scheme` 反而会信对端自报的说法。
     * @param isOverTls 本连接是否跑在 TLS 上（`HttpRequest::overTls()`）
     * @param authority 本连接的权威段原文：h1 的 `Host` 头部、h2/h3 的 `:authority`（缺席时补 host）
     * @return std::string 形如 `https://example.com:8443` 的源文本；authority 为空时返回空串，
     *         调用方据此知道「本连接无从判定同源」
     */
    [[nodiscard]] ASYN_NET_API std::string makeWebSocketSameOrigin(bool isOverTls, std::string_view authority);

    /**
     * @brief 一次升级请求按策略判完的结论
     *
     * @details 三条通道各自拼自己的应答（h1 是 101 报文、h2/h3 是 HEADERS 块），但**判**的部分只有一份：
     *          把判定写成一处，「同一份请求走不同通道得出不同结论」就没有可发生的位置。
     */
    struct ASYN_NET_API WebSocketUpgradeDecision
    {
        bool                       isAccepted{};          ///< 可以升级：会话继续发 101 / 200
        int                        rejectionStatus{};     ///< 被拒时给对端的状态码（403 来源不可接受、400 子协议谈不成且策略要求拒）
        std::string                rejectionReason{};     ///< 被拒时的中文原因，同时进正文与日志
        std::optional<std::string> selectedSubprotocol{}; ///< 谈成的子协议原文；空表示不回显（未参与协商，或谈不成但策略允许继续）
    };

    /**
     * @brief 按策略判一条升级请求：Origin 准入 + 子协议协商，一次判完
     *
     * @details 判据本体是 `isWebSocketOriginAllowed()` 与 `selectWebSocketSubprotocol()`，本函数只把它们
     *          串成一条通道要的那一份结论（谁先拒、拒了给什么码）。顺序是有意的：来源不可接受要先于
     *          子协议判——对端不该从「400 谈不成」与「403 来源不对」的差异里试探哪些源被放行。
     *
     * @param originValue 请求里 `Origin` 的原文，缺席时传空视图
     * @param offeredSubprotocols 请求里 `Sec-WebSocket-Protocol` 的原文，缺席时传空视图
     * @param sameOrigin 本连接的源（`makeWebSocketSameOrigin()` 拼出来的那一份）
     * @param policy 生效的策略
     * @return WebSocketUpgradeDecision 结论；`isAccepted` 为假时 `rejectionStatus` 与 `rejectionReason` 都已填好
     */
    [[nodiscard]] ASYN_NET_API WebSocketUpgradeDecision evaluateWebSocketUpgrade(std::string_view originValue, std::string_view offeredSubprotocols, std::string_view sameOrigin,
                                                                                 const WebSocketUpgradePolicy &policy);

    /**
     * @brief 判断一段文本是不是一个能解析的源（`scheme://host[:端口]`）
     *
     * @details 给登记策略的那一层用：`allowedOrigins` 里的表项写歪了（带了路径、协议名不对）会在
     *          每一次握手比对里悄悄落空，业务只看到「403 不知道为什么」——所以表项要在**登记那一刻**
     *          判形状，而不是等到握手时。判定与握手共用同一份解析，两处不会分叉。
     *
     * @param originText 待判的源文本，如 `https://example.com:8443`
     * @return true 是 http/https 的源，且主机与端口都读得出来
     * @return false 形状不合，调用方应拒收这份策略而不是留着它
     */
    [[nodiscard]] ASYN_NET_API bool isWebSocketOriginTextValid(std::string_view originText) noexcept;

    /**
     * @brief 按对端给出的次序挑一个本端愿意讲的子协议
     *
     * @details RFC 6455 §4.1 第 10 条规定客户端的列表「ordered by preference」，所以这里是**按客户端
     *          次序**取第一个本端候选表里有的，而不是按服务端偏好——同一条请求换承载通道不该挑出不同的
     *          子协议。对端没发这条头部、或一条都没谈成时返回空，调用方按
     *          `WebSocketSubprotocolMismatch` 决定是「不回显继续握手」还是拒掉。
     *
     * @param offeredValue 请求里 `Sec-WebSocket-Protocol` 的原文（逗号分隔的 token 列表），缺席时传空视图
     * @param policy 生效的策略，读它的 subprotocolCandidates
     * @return std::optional<std::string> 选中的子协议原文（逐字取自对端，不做大小写折叠——token 的
     *         比较不折叠，RFC 6455 §4.1 把它写成「必须互不相同的非空字符串」）；没得选时为空
     */
    [[nodiscard]] ASYN_NET_API std::optional<std::string> selectWebSocketSubprotocol(std::string_view offeredValue, const WebSocketUpgradePolicy &policy);
} // namespace AsynGyanis::Net
