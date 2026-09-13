// TestWebSocketHandshake.cpp —— WebSocket 握手（RFC 6455 §4）的单元测试
//
// 覆盖三块：Accept 值的 RFC 黄金样本、升级请求六条校验的通过与拒绝面（每条都要给出可操作的中文
// 原因）、101 应答报文的逐字节形态。用例都是纯计算，不起网络、不依赖任何外部服务。

#include "Net/WebSocket/WebSocketHandshake.h"

#include "Net/Http/HttpMethod.h"
#include "Net/Http/HttpRequest.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
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
        constexpr std::string_view kSecondClientKey = "x3JJHMbDL1EzLkh9GBhXDw==";
        constexpr std::string_view kSecondAcceptValue = "HSmrc0sMlYUkAGmm5OPpG2HaGWk=";

        /// 空 key 的期望值：此时参与摘要的只有 GUID 本身（外部工具算出）
        constexpr std::string_view kEmptyKeyAcceptValue = "Kfh9QIsMVZcl6xEPYxPHzW8SZ8w=";

        /**
         * @brief 判断文本里是否出现指定子串
         * @param haystack 待搜索文本
         * @param needle 目标子串
         * @return true 命中
         */
        bool containsText(const std::string &haystack, const std::string_view needle)
        {
            return haystack.find(needle) != std::string::npos;
        }

        /**
         * @brief 统计文本里指定子串出现的次数
         * @param haystack 待搜索文本
         * @param needle 目标子串，不得为空
         * @return std::size_t 出现次数
         */
        std::size_t countOccurrences(const std::string &haystack, const std::string_view needle)
        {
            std::size_t occurrenceCount = 0;
            for (std::size_t foundPosition = haystack.find(needle); foundPosition != std::string::npos;
                 foundPosition = haystack.find(needle, foundPosition + needle.size()))
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
        HttpRequest makeUpgradeRequestWith(const std::string_view upgradeValue, const std::string_view connectionValue,
                                           const std::string_view versionValue, const std::string_view keyValue)
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
        const std::vector<HttpMethod> nonGetMethods{HttpMethod::POST, HttpMethod::PUT, HttpMethod::DELETE, HttpMethod::HEAD,
                                                    HttpMethod::UNKNOWN};
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
            EXPECT_TRUE(containsText(rejectionReasonFor(makeUpgradeRequestWith(upgradeValue, "Upgrade", "13", kRfcExampleClientKey)),
                                     "Upgrade"))
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
            EXPECT_TRUE(
                    containsText(rejectionReasonFor(makeUpgradeRequestWith("websocket", connectionValue, "13", kRfcExampleClientKey)),
                                 "Connection"))
                    << "Connection 取值：" << connectionValue;
        }
    }

    /**
     * @brief Sec-WebSocket-Version 缺失或不是 13 时被拒，原因里要指出只支持 13
     */
    TEST(WebSocketHandshake, RejectsWebSocketVersionOtherThanThirteen)
    {
        for (const std::string_view versionValue: {"", "8", "12", "14"})
        {
            const std::string reason =
                    rejectionReasonFor(makeUpgradeRequestWith("websocket", "Upgrade", versionValue, kRfcExampleClientKey));

            EXPECT_TRUE(containsText(reason, "Sec-WebSocket-Version")) << "版本取值：" << versionValue;
            EXPECT_TRUE(containsText(reason, "13")) << "原因里必须写清只支持 13，版本取值：" << versionValue;
        }
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
                "dGhlIHNhbXBsZSBub25jZQ=*",   // 字母表外的字符 '*' 出现在末尾
                "dGhlIHNhbXBsZSBub25jZQ",     // 缺一个填充符，长度不是 4 的倍数
                "dGhlIHNhbXBsZSBub25jZQ===",  // 填充符过多
                "AAAAAAAAAAAAAAAAAAAAAP==",   // 填充位被置位，同一个字节串会有多种写法
                "dGhlIHNhbXBsZS Bub25jZQ==",  // 值里夹了空格
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

        const std::string expected = std::string("HTTP/1.1 101 Switching Protocols\r\n") + "Upgrade: websocket\r\n" +
                                     "Connection: Upgrade\r\n" + "Sec-WebSocket-Accept: " + std::string(kRfcExampleAcceptValue) + "\r\n" +
                                     "\r\n";
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
} // namespace AsynGyanis::Net
