// TestWebSocketHandshake.cpp —— WebSocket 握手（RFC 6455 §4）的单元测试
//
// 覆盖四块：Accept 值的 RFC 黄金样本、升级请求六条校验的通过与拒绝面（每条都要给出可操作的中文
// 原因）、101 应答报文的逐字节形态，以及出站客户端方向的两件事——升级请求的逐字节构造（含
// 拒绝能被用来断行的输入）与 §4.2.2 五条核对的通过与拒绝面。用例都是纯计算，不起网络、不依赖
// 任何外部服务；accept 的期望值取 RFC 原文给定的那一对样本，不是拿本端函数反推。

#include "Net/WebSocket/WebSocketHandshake.h"

#include "Net/Http/HttpMethod.h"
#include "Net/Http/HttpRequest.h"

#include "NetTestSupport.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// RFC 6455 §1.3 给出的黄金样本：客户端 Sec-WebSocket-Key
        constexpr std::string_view kRfcExampleClientKey = "dGhlIHNhbXBsZSBub25jZQ==";

        /// 与上面那枚 key 配对的 Accept 值（RFC 6455 §1.3 原文给出）
        constexpr std::string_view kRfcExampleAcceptValue = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

        /// 另一组由外部工具（python hashlib + base64）按同一公式算出的固定样本，
        /// 用来证明实现没有把黄金样本「硬编码成答案」
        constexpr std::string_view kSecondClientKey   = "x3JJHMbDL1EzLkh9GBhXDw==";
        constexpr std::string_view kSecondAcceptValue = "HSmrc0sMlYUkAGmm5OPpG2HaGWk=";

        /// 空 key 的期望值：此时参与摘要的只有 GUID 本身（外部工具算出）
        constexpr std::string_view kEmptyKeyAcceptValue = "Kfh9QIsMVZcl6xEPYxPHzW8SZ8w=";

        /**
         * @brief 判断文本里是否出现指定子串（定义见 NetTestSupport.h）
         */
        using AsynGyanis::Net::TestSupport::containsText;

        /**
         * @brief 统计文本里指定子串出现的次数
         * @param haystack 待搜索文本
         * @param needle 目标子串，不得为空
         * @return std::size_t 出现次数
         */
        std::size_t countOccurrences(const std::string &haystack, const std::string_view needle)
        {
            std::size_t occurrenceCount = 0;
            for (std::size_t foundPosition = haystack.find(needle); foundPosition != std::string::npos; foundPosition = haystack.find(needle, foundPosition + needle.size()))
            {
                ++occurrenceCount;
            }
            return occurrenceCount;
        }

        /**
         * @brief 按需裁剪地组装一条升级请求：值传空串表示不加该头部
         * @param upgradeValue Upgrade 头的值
         * @param connectionValue Connection 头的值
         * @param versionValue Sec-WebSocket-Version 头的值
         * @param keyValue Sec-WebSocket-Key 头的值
         * @return HttpRequest 组装好的 GET /chat HTTP/1.1 请求
         */
        HttpRequest makeUpgradeRequestWith(const std::string_view upgradeValue, const std::string_view connectionValue, const std::string_view versionValue,
                                           const std::string_view keyValue)
        {
            HttpRequest request;
            request.setMethod(HttpMethod::GET);
            request.setHttpVersion("HTTP/1.1");
            request.setUri("/chat");
            request.addHeader("host", "test");
            if (!upgradeValue.empty())
            {
                request.addHeader("upgrade", std::string(upgradeValue));
            }
            if (!connectionValue.empty())
            {
                request.addHeader("connection", std::string(connectionValue));
            }
            if (!versionValue.empty())
            {
                request.addHeader("sec-websocket-version", std::string(versionValue));
            }
            if (!keyValue.empty())
            {
                request.addHeader("sec-websocket-key", std::string(keyValue));
            }
            return request;
        }

        /**
         * @brief 组装一条「六条校验全部满足」的升级请求
         * @details 拒绝面用例在此基础上只改动要测的那一项，这样断言针对的就是那一项本身，
         *          而不是别的项顺带把请求拒了。
         * @return HttpRequest 合法的升级请求
         */
        HttpRequest makeValidUpgradeRequest()
        {
            return makeUpgradeRequestWith("websocket", "Upgrade", "13", kRfcExampleClientKey);
        }

        /**
         * @brief 断言请求被拒，并交出给出的中文原因
         * @param request 待校验的请求
         * @return std::string 失败原因，已断言非空
         */
        std::string rejectionReasonFor(const HttpRequest &request)
        {
            std::string failureReason("上一次失败留下的原因");
            EXPECT_FALSE(isWebSocketUpgradeRequest(request, &failureReason));
            EXPECT_FALSE(failureReason.empty()) << "失败必须给出原因，调用方据此排查";
            return failureReason;
        }
    } // namespace

    // ============================================================================
    // Accept 值：RFC 6455 §1.3 的黄金样本
    // ============================================================================

    /**
     * @brief 钉住 RFC 6455 §1.3 的黄金样本：示例 key 必须算出示例 Accept 值
     * @details 这组值是规范正文里印出来的，实现改了 GUID、摘要算法或 base64 字母表都会当场失败。
     */
    TEST(WebSocketHandshake, AcceptValueMatchesRfc6455GoldenSample)
    {
        EXPECT_EQ(computeWebSocketAcceptValue(kRfcExampleClientKey), kRfcExampleAcceptValue);
    }

    /**
     * @brief 换一枚 key 也要对上外部工具算出的固定值，证明实现不是只认黄金样本
     */
    TEST(WebSocketHandshake, AcceptValueMatchesExternallyComputedValueForAnotherKey)
    {
        EXPECT_EQ(computeWebSocketAcceptValue(kSecondClientKey), kSecondAcceptValue);
    }

    /**
     * @brief 空 key 也要算出「只有 GUID 参与摘要」的值，钉住拼接是无分隔符的直接首尾相拼
     */
    TEST(WebSocketHandshake, AcceptValueForEmptyKeyHashesTheGuidAlone)
    {
        EXPECT_EQ(computeWebSocketAcceptValue(""), kEmptyKeyAcceptValue);
    }

    // ============================================================================
    // 升级请求校验：通过与六条拒绝面
    // ============================================================================

    /**
     * @brief 六条校验全部满足时通过，且失败原因出参被清成空串（成功不残留上一次的原因）
     */
    TEST(WebSocketHandshake, AcceptsMinimalUpgradeRequestAndClearsReason)
    {
        std::string failureReason("上一次失败留下的原因");

        EXPECT_TRUE(isWebSocketUpgradeRequest(makeValidUpgradeRequest(), &failureReason));
        EXPECT_TRUE(failureReason.empty()) << "成功时出参必须为空，否则调用方会误判本次失败";
    }

    /**
     * @brief 不传失败原因出参时同样只给结论：成功与失败各一条，不允许崩溃
     */
    TEST(WebSocketHandshake, ToleratesNullFailureReason)
    {
        EXPECT_TRUE(isWebSocketUpgradeRequest(makeValidUpgradeRequest(), nullptr));
        EXPECT_FALSE(isWebSocketUpgradeRequest(HttpRequest{}, nullptr));
    }

    /**
     * @brief 非 GET 方法被拒，原因里要写清要求 GET
     */
    TEST(WebSocketHandshake, RejectsNonGetMethodWithActionableReason)
    {
        const std::vector<HttpMethod> nonGetMethods{HttpMethod::POST, HttpMethod::PUT, HttpMethod::DELETE, HttpMethod::HEAD, HttpMethod::UNKNOWN};
        for (const HttpMethod method: nonGetMethods)
        {
            HttpRequest request = makeValidUpgradeRequest();
            request.setMethod(method);

            EXPECT_TRUE(containsText(rejectionReasonFor(request), "GET")) << "原因里要写清本协议要求 GET";
        }
    }

    /**
     * @brief HTTP/1.0 及更早的版本被拒，原因里要指出要求 1.1 及以上
     */
    TEST(WebSocketHandshake, RejectsHttpVersionBelowEleven)
    {
        for (const std::string version: {"HTTP/1.0", "HTTP/0.9"})
        {
            HttpRequest request = makeValidUpgradeRequest();
            request.setHttpVersion(version);

            EXPECT_TRUE(containsText(rejectionReasonFor(request), "HTTP/1.1")) << "版本：" << version;
        }
    }

    /**
     * @brief 读不懂的版本串被拒，且不会悄悄按默认版本放行
     */
    TEST(WebSocketHandshake, RejectsUnparsableHttpVersion)
    {
        for (const std::string version: {"", "HTTP/1", "HTTP/1.", "HTTP/x.y", "1.1"})
        {
            HttpRequest request = makeValidUpgradeRequest();
            request.setHttpVersion(version);

            EXPECT_TRUE(containsText(rejectionReasonFor(request), "版本")) << "版本：" << version;
        }
    }

    /**
     * @brief 缺 Upgrade 头、或 Upgrade 里没有 websocket token 时被拒
     */
    TEST(WebSocketHandshake, RejectsMissingOrForeignUpgradeToken)
    {
        for (const std::string_view upgradeValue: {"", "h2c", "websocket-x"})
        {
            EXPECT_TRUE(containsText(rejectionReasonFor(makeUpgradeRequestWith(upgradeValue, "Upgrade", "13", kRfcExampleClientKey)), "Upgrade"))
                    << "Upgrade 取值：" << upgradeValue;
        }
    }

    /**
     * @brief Connection 头不含 Upgrade token 时被拒（只有这样中间代理才会放行这次协议切换）
     */
    TEST(WebSocketHandshake, RejectsConnectionHeaderWithoutUpgradeToken)
    {
        for (const std::string_view connectionValue: {"", "keep-alive", "close"})
        {
            EXPECT_TRUE(containsText(rejectionReasonFor(makeUpgradeRequestWith("websocket", connectionValue, "13", kRfcExampleClientKey)), "Connection"))
                    << "Connection 取值：" << connectionValue;
        }
    }

    /**
     * @brief Sec-WebSocket-Version 缺失或不是 13 时被拒，原因里要指出只支持 13，分类要落在版本那一档
     * @details 分类是会话侧决定「要不要补一条 Sec-WebSocket-Version: 13」的唯一依据（RFC 6455 §4.2.2），
     *          所以版本这两条出口必须判成 UnsupportedVersion，而 key 那类失败必须判成 Other——
     *          混在一起就会出现「key 不对却回答版本问题」这种把客户端引偏的应答。
     */
    TEST(WebSocketHandshake, RejectsWebSocketVersionOtherThanThirteen)
    {
        for (const std::string_view versionValue: {"", "8", "12", "14"})
        {
            const HttpRequest           request = makeUpgradeRequestWith("websocket", "Upgrade", versionValue, kRfcExampleClientKey);
            std::string                 reason;
            WebSocketHandshakeRejection rejection{WebSocketHandshakeRejection::None};
            EXPECT_FALSE(isWebSocketUpgradeRequest(request, &reason, &rejection)) << "版本取值：" << versionValue;
            EXPECT_EQ(rejection, WebSocketHandshakeRejection::UnsupportedVersion) << "版本取值：" << versionValue << "，分类错了会话就不会回那条版本头部";

            EXPECT_TRUE(containsText(reason, "Sec-WebSocket-Version")) << "版本取值：" << versionValue;
            EXPECT_TRUE(containsText(reason, "13")) << "原因里必须写清只支持 13，版本取值：" << versionValue;
        }
    }

    /**
     * @brief 缺 key 的拒绝不得判成版本类：那条 Sec-WebSocket-Version 头部不是它的义务
     */
    TEST(WebSocketHandshake, ClassifiesMissingKeyRejectionAsOtherThanVersion)
    {
        const HttpRequest           request = makeUpgradeRequestWith("websocket", "Upgrade", "13", "");
        std::string                 reason;
        WebSocketHandshakeRejection rejection{WebSocketHandshakeRejection::None};
        EXPECT_FALSE(isWebSocketUpgradeRequest(request, &reason, &rejection));
        EXPECT_EQ(rejection, WebSocketHandshakeRejection::Other) << "缺 key 被判成版本类，应答会伪装成版本问题";

        // 反向对照：同一份请求把 key 补上就应当通过，且分类落回 None——否则上面的 Other 可能只是「恒不通过」
        const HttpRequest good = makeUpgradeRequestWith("websocket", "Upgrade", "13", kRfcExampleClientKey);
        EXPECT_TRUE(isWebSocketUpgradeRequest(good, nullptr, &rejection));
        EXPECT_EQ(rejection, WebSocketHandshakeRejection::None);
    }

    /**
     * @brief Sec-WebSocket-Key 缺失时被拒
     */
    TEST(WebSocketHandshake, RejectsMissingWebSocketKey)
    {
        EXPECT_TRUE(containsText(rejectionReasonFor(makeUpgradeRequestWith("websocket", "Upgrade", "13", "")), "Sec-WebSocket-Key"));
    }

    /**
     * @brief key 是合法 base64 但解码后不是 16 字节时被拒，原因里要给出实际字节数与要求
     */
    TEST(WebSocketHandshake, RejectsKeyWhoseDecodedLengthIsNotSixteenBytes)
    {
        // 15 字节（20 字符）与 17 字节（24 字符）各一条：长度两边的偏差都要拦住
        for (const std::string_view keyValue: {"MDEyMzQ1Njc4OWFiY2Rl", "MDEyMzQ1Njc4OWFiY2RlZmc="})
        {
            const std::string reason = rejectionReasonFor(makeUpgradeRequestWith("websocket", "Upgrade", "13", keyValue));

            EXPECT_TRUE(containsText(reason, "16")) << "key：" << keyValue;
            EXPECT_TRUE(containsText(reason, "base64")) << "原因里要写清正确的形态，key：" << keyValue;
        }
    }

    /**
     * @brief 不是规范 base64 的 key 一律被拒：非法字符、长度不是 4 的倍数、填充位被置位
     */
    TEST(WebSocketHandshake, RejectsKeyThatIsNotCanonicalBase64)
    {
        const std::vector<std::string> invalidKeys{
                "dGhlIHNhbXBsZSBub25jZQ=*",  // 字母表外的字符 '*' 出现在末尾
                "dGhlIHNhbXBsZSBub25jZQ",    // 缺一个填充符，长度不是 4 的倍数
                "dGhlIHNhbXBsZSBub25jZQ===", // 填充符过多
                "AAAAAAAAAAAAAAAAAAAAAP==",  // 填充位被置位，同一个字节串会有多种写法
                "dGhlIHNhbXBsZS Bub25jZQ==", // 值里夹了空格
        };
        for (const std::string &keyValue: invalidKeys)
        {
            const std::string reason = rejectionReasonFor(makeUpgradeRequestWith("websocket", "Upgrade", "13", keyValue));

            EXPECT_TRUE(containsText(reason, "base64")) << "key：" << keyValue;
        }
    }

    /**
     * @brief token 比对大小写不敏感、按逗号拆分，版本值前后的可选空白也容忍
     */
    TEST(WebSocketHandshake, MatchesTokensCaseInsensitivelyAcrossCommaList)
    {
        const HttpRequest request = makeUpgradeRequestWith("WebSocket, foo", "keep-alive, Upgrade", " 13 ", kRfcExampleClientKey);

        EXPECT_TRUE(isWebSocketUpgradeRequest(request, nullptr));
    }

    // ============================================================================
    // 101 应答报文：逐字节形态
    // ============================================================================

    /**
     * @brief 钉住 101 报文的逐字节形态：状态行、三条头部、CRLF 与结束空行
     */
    TEST(WebSocketHandshake, BuildsExactlyTheExpected101ResponseBytes)
    {
        const std::string response = buildHandshakeResponse(kRfcExampleClientKey);

        const std::string expected = std::string("HTTP/1.1 101 Switching Protocols\r\n") + "Upgrade: websocket\r\n" + "Connection: Upgrade\r\n" +
                                     "Sec-WebSocket-Accept: " + std::string(kRfcExampleAcceptValue) + "\r\n" + "\r\n";
        EXPECT_EQ(response, expected);
    }

    /**
     * @brief 报文只协商协议切换本身：不含扩展与子协议，且恰有三条头部
     */
    TEST(WebSocketHandshake, NegotiatesNoExtensionsOrSubProtocols)
    {
        const std::string response = buildHandshakeResponse(kRfcExampleClientKey);

        EXPECT_TRUE(response.starts_with("HTTP/1.1 101 "));
        EXPECT_TRUE(response.ends_with("\r\n\r\n")) << "头部块必须以空行收尾，否则对端会把后续帧字节当头部读";
        EXPECT_EQ(response.find("Sec-WebSocket-Extensions"), std::string::npos) << "不协商扩展，RSV 位因此必须为 0";
        EXPECT_EQ(response.find("Sec-WebSocket-Protocol"), std::string::npos) << "不协商子协议";

        // 状态行 1 个 CRLF + 三条头部各 1 个 + 结束空行 1 个
        EXPECT_EQ(countOccurrences(response, "\r\n"), 5U);
    }

    /**
     * @brief 换一枚 key 应答里的 Accept 行跟着变（报文不是常量串）
     */
    TEST(WebSocketHandshake, ResponseCarriesTheAcceptValueOfTheGivenKey)
    {
        const std::string response = buildHandshakeResponse(kSecondClientKey);

        EXPECT_TRUE(containsText(response, "Sec-WebSocket-Accept: " + std::string(kSecondAcceptValue) + "\r\n"));
    }
    /**
     * @brief 钉住未协商扩展时不写 Sec-WebSocket-Extensions
     * @details 声明一个对端没用过的扩展会让它按压缩发帧、而本端按明文读，线上必然错位
     */
    TEST(WebSocketHandshake, OmitsExtensionsHeaderWhenNotNegotiated)
    {
        const std::string response = buildHandshakeResponse(kRfcExampleClientKey);
        EXPECT_EQ(response.find("Sec-WebSocket-Extensions"), std::string::npos) << "没协商就不该声明扩展，实际：" << response;
    }

    /**
     * @brief 钉住协商成功时把结论逐字写进 101，且不影响 Accept 值与结束空行
     */
    TEST(WebSocketHandshake, WritesExtensionsHeaderWhenNegotiated)
    {
        constexpr std::string_view kNegotiatedValue = "permessage-deflate; server_no_context_takeover; client_no_context_takeover";

        const std::string response     = buildHandshakeResponse(kRfcExampleClientKey, kNegotiatedValue);
        const std::string expectedLine = std::string("Sec-WebSocket-Extensions: ") + std::string(kNegotiatedValue) + "\r\n";
        ASSERT_NE(response.find(expectedLine), std::string::npos) << "协商结论必须逐字出现在 101 里，实际：" << response;
        EXPECT_EQ(response.find("Sec-WebSocket-Extensions"), response.rfind("Sec-WebSocket-Extensions")) << "扩展只能声明一次";
        EXPECT_NE(response.find("Sec-WebSocket-Accept: "), std::string::npos) << "扩展头不能挤掉 Accept：" << response;
        EXPECT_TRUE(response.ends_with("\r\n\r\n")) << "结束空行不能被扩展头挤掉：" << response;
    }

    // ============================================================================
    // 出站客户端方向：升级请求的构造与 101 应答的核对
    // ============================================================================

    /**
     * @brief 钉住必发六行的逐字节形态，以及「没提议就不发可选头部」
     * @details 期望值按 RFC 6455 §4.1 的字段顺序手写，不是拿被测的拼装结果反推——否则改顺序
     *          与漏发头部这两类坏都红不出来。
     */
    TEST(WebSocketHandshake, UpgradeRequestCarriesExactlyTheMandatoryLines)
    {
        const auto request = buildWebSocketUpgradeRequest("example.com:8443", "/chat?room=1", kRfcExampleClientKey);

        ASSERT_TRUE(request.has_value()) << request.error();
        EXPECT_EQ(*request, "GET /chat?room=1 HTTP/1.1\r\n"
                            "Host: example.com:8443\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                            "Sec-WebSocket-Version: 13\r\n"
                            "\r\n");
    }

    /**
     * @brief 钉住两条可选头部只在有要约时出现，且顺序与结束空行都不被挤掉
     */
    TEST(WebSocketHandshake, UpgradeRequestAppendsOfferedSubprotocolsAndExtensions)
    {
        const auto request = buildWebSocketUpgradeRequest("example.com", "/ws", kRfcExampleClientKey, std::vector<std::string>{"chat", "superchat"},
                                                          "permessage-deflate; client_max_window_bits=12");

        ASSERT_TRUE(request.has_value()) << request.error();
        EXPECT_NE(request->find("Sec-WebSocket-Protocol: chat, superchat\r\n"), std::string::npos) << *request;
        EXPECT_NE(request->find("Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits=12\r\n"), std::string::npos) << *request;
        EXPECT_TRUE(request->ends_with("\r\n\r\n")) << "结束空行必须还是那一个空行：" << *request;
    }

    /**
     * @brief 钉住三段由调用方拼进来的文本都不许带断行符或空白
     * @details 这类输入来自 URL 或配置，最容易混进 CR/LF；悄悄去掉等于替调用方发了一条它没打算发的
     *          请求，因此一律拒绝并把不合用的那一段点名。
     */
    TEST(WebSocketHandshake, UpgradeRequestRejectsHeaderSplittingInput)
    {
        if (const auto injectedHost = buildWebSocketUpgradeRequest("example.com\r\nX-Injected: 1", "/ws", kRfcExampleClientKey); injectedHost.has_value())
        {
            FAIL() << "Host 里的 CRLF 必须被拒掉，而不是被清洗后发出";
        } else
        {
            EXPECT_TRUE(injectedHost.error().find("Host") != std::string::npos) << injectedHost.error();
        }

        if (const auto spacedTarget = buildWebSocketUpgradeRequest("example.com", "/a b", kRfcExampleClientKey); spacedTarget.has_value())
        {
            FAIL() << "请求目标里的空格会把请求行拆成三段，必须拒";
        }

        if (const auto badSubprotocol = buildWebSocketUpgradeRequest("example.com", "/ws", kRfcExampleClientKey, std::vector<std::string>{"bad name"}); badSubprotocol.has_value())
        {
            FAIL() << "子协议名不是合法 token，必须拒而不是照发";
        }
    }

    /**
     * @brief 钉住一次合格的 101 握手被核对通过，且没有要约时两个结论都是空/假
     */
    TEST(WebSocketHandshake, UpgradeResponseAcceptsRfcGoldenHandshake)
    {
        const std::vector<std::pair<std::string, std::string>> headers{
                {"Upgrade", "WebSocket"}, {"Connection", "keep-alive, Upgrade"}, {"sec-websocket-accept", std::string(kRfcExampleAcceptValue)}};

        const auto agreement = validateWebSocketUpgradeResponse(101, headers, kRfcExampleClientKey);

        ASSERT_TRUE(agreement.has_value()) << agreement.error();
        EXPECT_TRUE(agreement->acceptedSubprotocol.empty());
        EXPECT_FALSE(agreement->isPerMessageDeflateAccepted);
    }

    /**
     * @brief 逐条钉住 §4.2.2 的拒绝面：每条都要出声且点名判不过的是哪一条
     */
    TEST(WebSocketHandshake, UpgradeResponseRejectsEveryFailedValidationCheck)
    {
        const std::vector<std::pair<std::string, std::string>> goodHeaders{
                {"Upgrade", "websocket"}, {"Connection", "Upgrade"}, {"sec-websocket-accept", std::string(kRfcExampleAcceptValue)}};

        // 状态码不是 101：升级不成立，本端要按普通 HTTP 响应处理
        EXPECT_FALSE(validateWebSocketUpgradeResponse(200, goodHeaders, kRfcExampleClientKey).has_value());

        // Upgrade 缺 / 值不是 websocket
        EXPECT_FALSE(validateWebSocketUpgradeResponse(101, {{"Connection", "Upgrade"}, {"sec-websocket-accept", std::string(kRfcExampleAcceptValue)}}, kRfcExampleClientKey)
                             .has_value());
        if (const auto wrongUpgrade = validateWebSocketUpgradeResponse(
                    101, {{"Upgrade", "h2c"}, {"Connection", "Upgrade"}, {"sec-websocket-accept", std::string(kRfcExampleAcceptValue)}}, kRfcExampleClientKey);
            wrongUpgrade.has_value())
        {
            FAIL() << "Upgrade 升的不是 websocket 时必须拒";
        }

        // Connection 里没有 upgrade 这个 token（列表里得有它，中间设施才会放行协议切换）
        if (const auto badConnection = validateWebSocketUpgradeResponse(
                    101, {{"Upgrade", "websocket"}, {"Connection", "keep-alive"}, {"sec-websocket-accept", std::string(kRfcExampleAcceptValue)}}, kRfcExampleClientKey);
            badConnection.has_value())
        {
            FAIL() << "Connection 缺 upgrade token 时必须拒";
        } else
        {
            EXPECT_TRUE(badConnection.error().find("Connection") != std::string::npos) << badConnection.error();
        }

        // Accept 缺、错值、以及同名两条：都不成立
        EXPECT_FALSE(validateWebSocketUpgradeResponse(101, {{"Upgrade", "websocket"}, {"Connection", "Upgrade"}}, kRfcExampleClientKey).has_value());
        if (const auto wrongAccept = validateWebSocketUpgradeResponse(
                    101, {{"Upgrade", "websocket"}, {"Connection", "Upgrade"}, {"sec-websocket-accept", "AAAAAAAAAAAAAAAAAAAAAAAAAAA="}}, kRfcExampleClientKey);
            wrongAccept.has_value())
        {
            FAIL() << "accept 与本端算出的值不一致时必须拒——这是「这条应答是不是回给我这次握手」的唯一信号";
        }
        if (const auto doubledAccept = validateWebSocketUpgradeResponse(101,
                                                                        {{"Upgrade", "websocket"},
                                                                         {"Connection", "Upgrade"},
                                                                         {"sec-websocket-accept", std::string(kRfcExampleAcceptValue)},
                                                                         {"Sec-WebSocket-Accept", std::string(kRfcExampleAcceptValue)}},
                                                                        kRfcExampleClientKey);
            doubledAccept.has_value())
        {
            FAIL() << "两条 accept 意味着应答不属于这次握手，不能挑一条信";
        }
    }

    /**
     * @brief 钉住回显结论会被落地：选中的子协议与接受的扩展都要交回会话层
     */
    TEST(WebSocketHandshake, UpgradeResponseReportsSelectedSubprotocolAndDeflate)
    {
        const std::vector<std::pair<std::string, std::string>> headers{{"Upgrade", "websocket"},
                                                                       {"Connection", "Upgrade"},
                                                                       {"sec-websocket-accept", std::string(kRfcExampleAcceptValue)},
                                                                       {"Sec-WebSocket-Protocol", " chat "},
                                                                       {"Sec-WebSocket-Extensions", "permessage-deflate; server_no_context_takeover"}};

        const auto agreement = validateWebSocketUpgradeResponse(101, headers, kRfcExampleClientKey, "permessage-deflate; client_max_window_bits=12");

        ASSERT_TRUE(agreement.has_value()) << agreement.error();
        EXPECT_EQ(agreement->acceptedSubprotocol, "chat") << "回显值两侧的空白不属于内容，要折掉再交回";
        EXPECT_TRUE(agreement->isPerMessageDeflateAccepted);
    }

    /**
     * @brief 钉住「回显了本端没提议的扩展」判失败（RFC 6455 §9.1）
     * @details 这一条不能放行：接受一个自己没提议的扩展等于让对面决定本端的收发光景，
     *          而本端连那个扩展的实现都没有。
     */
    TEST(WebSocketHandshake, UpgradeResponseRejectsExtensionThatWasNotOffered)
    {
        const std::vector<std::pair<std::string, std::string>> headers{{"Upgrade", "websocket"},
                                                                       {"Connection", "Upgrade"},
                                                                       {"sec-websocket-accept", std::string(kRfcExampleAcceptValue)},
                                                                       {"Sec-WebSocket-Extensions", "permessage-deflate"}};

        if (const auto agreement = validateWebSocketUpgradeResponse(101, headers, kRfcExampleClientKey); agreement.has_value())
        {
            FAIL() << "本端没提议扩展时，对面回了 permessage-deflate 必须判失败";
        } else
        {
            EXPECT_TRUE(agreement.error().find("没提议") != std::string::npos) << agreement.error();
        }
    }

    /**
     * @brief 钉住列表型头部按 token 比对、扩展名按整名比对
     * @details 两条都是「前缀命中不等于命中」的形状：Connection 写成 upgrademe 不算带了 upgrade
     *          这个 token（RFC 6455 §4.2.2 第 3 条要的是 token），对面回一个更长的扩展名也不是
     *          本端提议过的那个扩展（§9.1）。用子串比较的实现两条都放行，而线上表现是「握手成功
     *          但后续行为由对面决定」。
     */
    TEST(WebSocketHandshake, UpgradeResponseRequiresWholeTokensNotSubstrings)
    {
        if (const auto fakeConnection = validateWebSocketUpgradeResponse(
                    101, {{"Upgrade", "websocket"}, {"Connection", "upgrademe"}, {"sec-websocket-accept", std::string(kRfcExampleAcceptValue)}}, kRfcExampleClientKey);
            fakeConnection.has_value())
        {
            FAIL() << "Connection 里的 upgrademe 不是 upgrade 这个 token，不能放行";
        }

        if (const auto longerExtension = validateWebSocketUpgradeResponse(101,
                                                                          {{"Upgrade", "websocket"},
                                                                           {"Connection", "Upgrade"},
                                                                           {"sec-websocket-accept", std::string(kRfcExampleAcceptValue)},
                                                                           {"Sec-WebSocket-Extensions", "permessage-deflate-experimental"}},
                                                                          kRfcExampleClientKey, "permessage-deflate");
            longerExtension.has_value())
        {
            FAIL() << "回显的扩展名以本端提议者为前缀、但不是同一个名字，必须判失败";
        }
    }

    /**
     * @brief 钉住扩展参数可以不同但名字必须对得上：名字之后的参数由扩展自己判
     */
    TEST(WebSocketHandshake, UpgradeResponseComparesExtensionNameNotWholeParameterList)
    {
        const std::vector<std::pair<std::string, std::string>> headers{{"Upgrade", "websocket"},
                                                                       {"Connection", "Upgrade"},
                                                                       {"sec-websocket-accept", std::string(kRfcExampleAcceptValue)},
                                                                       {"Sec-WebSocket-Extensions", "permessage-deflate; client_no_context_takeover"}};

        const auto agreement = validateWebSocketUpgradeResponse(101, headers, kRfcExampleClientKey, "permessage-deflate");

        ASSERT_TRUE(agreement.has_value()) << agreement.error();
        EXPECT_TRUE(agreement->isPerMessageDeflateAccepted);
    }

} // namespace AsynGyanis::Net
