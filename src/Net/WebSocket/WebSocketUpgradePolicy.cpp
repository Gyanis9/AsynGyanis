#include "Net/WebSocket/WebSocketUpgradePolicy.h"

#include "Net/Http/Message/HttpHeaderRules.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 参与同源判定的那两个协议名（RFC 6454 §2.3 第 6 步要按协议补默认端口，别的协议补不出）
        constexpr std::string_view kHttpSchemeName  = "http";
        constexpr std::string_view kHttpsSchemeName = "https";

        /**
         * @brief 把协议名折成小写并判它是不是参与同源判定的那两个
         * @param schemeText 冒号之前那段原文
         * @return std::optional<std::string_view> 命中时交回小写形态的那一份常量；否则为空
         */
        std::optional<std::string_view> normalizeOriginScheme(const std::string_view schemeText)
        {
            if (equalsIgnoringCase(schemeText, kHttpSchemeName))
            {
                return kHttpSchemeName;
            }
            if (equalsIgnoringCase(schemeText, kHttpsSchemeName))
            {
                return kHttpsSchemeName;
            }
            return std::nullopt;
        }

        /**
         * @brief 把权威段拆成主机与端口两部分
         * @details 方括号那一支是 IPv6 字面量，端口分隔符只可能出现在右括号之后；非方括号那一支第一个
         *          冒号就是分隔符。多于一个冒号、`]` 之后不是冒号这些形状都判读不出来——本函数只拆
         *          结构，不校验语法（语法归 findAuthorityFieldDefect）。
         * @param authority 权威段原文（非空）
         * @param hostText 输出：主机部分，IPv6 连方括号一起留
         * @param portText 输出：端口部分原文，没有端口时为空
         * @return true 拆得出
         * @return false 形状读不出来，调用方按非法源处理
         */
        bool splitAuthority(const std::string_view authority, std::string_view &hostText, std::string_view &portText)
        {
            hostText = authority;
            portText = {};
            if (authority.empty())
            {
                return false;
            }
            if (authority.front() == '[')
            {
                const std::size_t closingBracket = authority.find(']');
                if (closingBracket == std::string_view::npos)
                {
                    return false;
                }
                hostText = authority.substr(0, closingBracket + 1);
                if (closingBracket + 1 == authority.size())
                {
                    return true;
                }
                if (authority[closingBracket + 1] != ':')
                {
                    return false;
                }
                portText = authority.substr(closingBracket + 2);
                return true;
            }
            const std::size_t colonPosition = authority.find(':');
            if (colonPosition != std::string_view::npos)
            {
                hostText = authority.substr(0, colonPosition);
                portText = authority.substr(colonPosition + 1);
                // 主机段里再出现冒号就不是一个能拆分的形式（IPv6 必须打方括号）
                return portText.find(':') == std::string_view::npos;
            }
            return true;
        }

        /**
         * @brief 把「scheme://host[:端口]」折成可比对的源键
         * @details 口径是 RFC 6454 §5：源是 (scheme, host, port) 三元组，全等才算同一个源。折法是
         *          scheme 与 host 走 ASCII 小写、结尾根点不参与（与本仓选站共用同一份归一化，见
         *          normalizeHostComparisonKey）；端口缺省时按 §2.3 第 6 步补成该协议的默认端口，
         *          补完正是要不要省略的判断，所以这里等价地把默认端口丢掉：`http://a.com` 与
         *          `http://a.com:80` 同一个源，`https://a.com:80` 与 `https://a.com` 不是。
         * @param originText 源文本原文
         * @return std::optional<std::string> 形如 `https://example.com:8443`（默认端口不带）；
         *         形状读不出来时为空，调用方据此判非法，不做任何猜测
         */
        std::optional<std::string> normalizeOriginText(const std::string_view originText)
        {
            const std::string_view trimmed           = trimOptionalWhitespace(originText);
            const std::size_t      separatorPosition = trimmed.find("://");
            if (separatorPosition == std::string_view::npos || separatorPosition == 0)
            {
                return std::nullopt;
            }

            const std::optional<std::string_view> scheme = normalizeOriginScheme(trimmed.substr(0, separatorPosition));
            if (!scheme.has_value())
            {
                return std::nullopt;
            }

            const std::string_view remainder = trimmed.substr(separatorPosition + 3);
            // 序列化后的源到端口为止（RFC 6454 §3 的 ABNF）：出现路径/查询/片段就是形状不合，
            // 悄悄截掉等于替对端编一个它没发出来的源
            if (remainder.empty() || remainder.find_first_of("/?#") != std::string_view::npos)
            {
                return std::nullopt;
            }

            std::string_view hostText{};
            std::string_view portText{};
            if (!splitAuthority(remainder, hostText, portText))
            {
                return std::nullopt;
            }
            const std::string hostKey = normalizeHostComparisonKey(hostText);
            if (hostKey.empty())
            {
                return std::nullopt;
            }
            if (portText.empty())
            {
                return std::format("{}://{}", *scheme, hostKey);
            }

            std::string portNumber(portText);
            for (const char digit: portNumber)
            {
                if (digit < '0' || digit > '9')
                {
                    return std::nullopt;
                }
            }
            // 前导零不参与端口比较：080 与 80 指的是同一个端口，写法没必要留在键里
            while (portNumber.size() > 1 && portNumber.front() == '0')
            {
                portNumber.erase(portNumber.begin());
            }
            const bool isDefaultPort = (portNumber == "80" && *scheme == kHttpSchemeName) || (portNumber == "443" && *scheme == kHttpsSchemeName);
            if (isDefaultPort)
            {
                return std::format("{}://{}", *scheme, hostKey);
            }
            return std::format("{}://{}:{}", *scheme, hostKey, portNumber);
        }
    } // namespace

    std::string makeWebSocketSameOrigin(const bool isOverTls, const std::string_view authority)
    {
        const std::string_view trimmed = trimOptionalWhitespace(authority);
        if (trimmed.empty())
        {
            return {};
        }
        // 端口原样带过去：缺省端口由 normalizeOriginText 按 RFC 6454 §2.3 第 6 步补，两处的口径因此一致
        return std::format("{}://{}", isOverTls ? kHttpsSchemeName : kHttpSchemeName, trimmed);
    }

    WebSocketUpgradeDecision evaluateWebSocketUpgrade(const std::string_view originValue, const std::string_view offeredSubprotocols, const std::string_view sameOrigin,
                                                      const WebSocketUpgradePolicy &policy)
    {
        WebSocketUpgradeDecision decision;

        // 来源不对先判：403 是 RFC 6455 §10.2 为「来源不可接受」备的那一档，而且不该让对端从
        // 「400 谈不成」与「403 来源不对」的差异里试探出哪些源被放行
        if (!isWebSocketOriginAllowed(originValue, sameOrigin, policy))
        {
            decision.isAccepted      = false;
            decision.rejectionStatus = 403;
            decision.rejectionReason = describeWebSocketOriginRejection(originValue, sameOrigin, policy);
            return decision;
        }

        decision.selectedSubprotocol = selectWebSocketSubprotocol(offeredSubprotocols, policy);
        if (decision.selectedSubprotocol.has_value() || policy.subprotocolCandidates.empty() || policy.subprotocolMismatch == WebSocketSubprotocolMismatch::ProceedWithoutEcho)
        {
            // 谈成，或策略允许「不回显继续握手」（RFC 6455 §4.2.2：谈不成时唯一可发的值是 null）
            decision.isAccepted = true;
            return decision;
        }

        // 走到这里：本端有候选表、一条都没谈成、而策略要求谈不成即拒
        std::string candidateText;
        for (const std::string &candidate: policy.subprotocolCandidates)
        {
            if (!candidateText.empty())
            {
                candidateText.append(", ");
            }
            candidateText.append(candidate);
        }
        decision.isAccepted      = false;
        decision.rejectionStatus = 400;
        decision.rejectionReason = std::format("WebSocket 升级被拒：对端提议的子协议 \"{}\" 与本端能讲的 [{}] 没有交集，"
                                               "而本端策略要求谈不成即拒。要么让对端改发其中之一，"
                                               "要么把 WebSocketUpgradePolicy::subprotocolMismatch 设回 ProceedWithoutEcho",
                                               offeredSubprotocols.empty() ? std::string_view("（对端没发这条头部）") : offeredSubprotocols, candidateText);
        return decision;
    }

    bool isWebSocketOriginTextValid(const std::string_view originText) noexcept
    {
        return normalizeOriginText(originText).has_value();
    }

    bool isWebSocketOriginAllowed(const std::string_view originValue, const std::string_view sameOrigin, const WebSocketUpgradePolicy &policy) noexcept
    {
        // AllowAny 是调用方显式选的「任何网页都能连」，这一档不再判任何形状
        if (policy.originMode == WebSocketOriginMode::AllowAny)
        {
            return true;
        }
        // Origin 缺席按放行：RFC 6455 §4.1 第 8 条把必发义务只派给浏览器客户端，非浏览器客户端是
        // 「MAY include」。判死会把自己家的 WebSocketClient、curl 与整条 Autobahn 套件挡在门外
        if (trimOptionalWhitespace(originValue).empty())
        {
            return true;
        }

        const std::optional<std::string> originKey = normalizeOriginText(originValue);
        if (!originKey.has_value())
        {
            // 形状不合一律拒：这条 Origin 想说哪个源我们读不出来，而「读不出来还放行」是把来源判定
            // 交给了发起方自己的写法
            return false;
        }

        if (policy.originMode == WebSocketOriginMode::AllowList)
        {
            // 表项在登记处校验过形状，这里比不上的那几条继续往下走同源那一档
            for (const std::string &allowedOrigin: policy.allowedOrigins)
            {
                if (const std::optional<std::string> allowedKey = normalizeOriginText(allowedOrigin); allowedKey.has_value() && *allowedKey == *originKey)
                {
                    return true;
                }
            }
        }

        // 同源那一档：本连接拼不出自己的源（会话没拿到权威段）时也判否——判据拿不准的一侧宁可拒，
        // 「无从比对」不等于「同源」
        const std::optional<std::string> sameKey = normalizeOriginText(sameOrigin);
        return sameKey.has_value() && *sameKey == *originKey;
    }

    std::string describeWebSocketOriginRejection(const std::string_view originValue, const std::string_view sameOrigin, const WebSocketUpgradePolicy &policy)
    {
        if (isWebSocketOriginAllowed(originValue, sameOrigin, policy))
        {
            return {};
        }

        const std::string_view trimmedOrigin = trimOptionalWhitespace(originValue);
        if (!normalizeOriginText(trimmedOrigin).has_value())
        {
            return std::format("WebSocket 握手被拒：Origin 取值 \"{}\" 不是一个能解析的源。RFC 6454 §3 规定序列化后的源是 "
                               "scheme://host[:端口]，scheme 只认 http 与 https，且不带路径、查询与片段；请把它写成这样的源，"
                               "或在策略里显式放开（WebSocketOriginMode::AllowList / AllowAny）",
                               trimmedOrigin);
        }

        if (policy.originMode == WebSocketOriginMode::AllowList)
        {
            std::string allowedText;
            for (const std::string &allowedOrigin: policy.allowedOrigins)
            {
                if (!allowedText.empty())
                {
                    allowedText.append(", ");
                }
                allowedText.append(allowedOrigin);
            }
            return std::format("WebSocket 握手被拒：Origin \"{}\" 既不是同源（{}）也不在放行表 [{}] 里。要接这个来源，把它的源写进 "
                               "WebSocketUpgradePolicy::allowedOrigins（形如 https://example.com，不带路径）",
                               trimmedOrigin, sameOrigin, allowedText);
        }

        return std::format("WebSocket 握手被拒：Origin \"{}\" 与同源（{}）不一致，当前策略只放同源。要接这个来源，把 "
                           "WebSocketUpgradePolicy::originMode 设成 AllowList 并列出可放行的源；确认要对任意网页开放才设 AllowAny",
                           trimmedOrigin, sameOrigin);
    }

    std::optional<std::string> selectWebSocketSubprotocol(const std::string_view offeredValue, const WebSocketUpgradePolicy &policy)
    {
        // 候选表为空表示本端不参与协商：回显一条对端没要求的名字是 RFC 6455 §4.2.2 步骤 6 里客户端
        // 必须判失败的形状，而「谈不回显」才是合法的落点
        if (policy.subprotocolCandidates.empty())
        {
            return std::nullopt;
        }

        std::optional<std::string> selected{};
        // 按对端给出的次序取第一个本端有的（RFC 6455 §4.1 第 10 条把列表写成「ordered by preference」）。
        // 比较逐字进行：子协议名是按 §11.5 注册的标识符，大小写不同就是两个名字
        static_cast<void>(anyTokenInList(offeredValue,
                                         [&](const std::string_view offeredToken)
                                         {
                                             if (offeredToken.empty())
                                             {
                                                 return false;
                                             }
                                             if (std::ranges::find(policy.subprotocolCandidates, offeredToken) == policy.subprotocolCandidates.end())
                                             {
                                                 return false;
                                             }
                                             selected.emplace(offeredToken);
                                             return true;
                                         }));
        return selected;
    }
} // namespace AsynGyanis::Net
