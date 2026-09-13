/**
 * @file TestPerMessageDeflate.cpp
 * @brief permessage-deflate 用例：扩展协商、消息往返、不可压内容与解压上限
 * @details 往返一律逐字节比较；解压上限那条单独钉住 zip bomb——压缩比可以做到几百倍，
 *          不设上限时一条小消息就能把服务端内存撑爆。
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Net/WebSocket/PerMessageDeflate.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 可压且够长的正文
        const std::string kRepetitivePayload = []
        {
            std::string payload;
            payload.reserve(8192);
            for (int index = 0; index < 256; ++index)
            {
                payload += "permessage-deflate RFC7692 payload/";
            }
            return payload;
        }();

        /**
         * @brief 造一段伪随机字节（几乎压不动）
         * @return std::string 字节
         */
        std::string makeIncompressiblePayload()
        {
            std::string bytes;
            bytes.reserve(4096);
            unsigned int state = 0x2545F491U;
            for (int index = 0; index < 4096; ++index)
            {
                state = state * 1664525U + 1013904223U;
                bytes.push_back(static_cast<char>((state >> 16) & 0xFF));
            }
            return bytes;
        }
    } // namespace

    /**
     * @brief 对端提供 permessage-deflate 时接受，并回本端选定的参数
     */
    TEST(PerMessageDeflate, AcceptsWhenClientOffersTheExtension)
    {
        const PerMessageDeflateNegotiation negotiation = negotiatePerMessageDeflate("permessage-deflate");

        ASSERT_TRUE(negotiation.accepted) << "对端提供了扩展却没收下";
        EXPECT_NE(negotiation.responseValue.find("permessage-deflate"), std::string::npos);
        // 两条 no_context_takeover 是本端选定的策略：要求每条消息重置上下文
        EXPECT_NE(negotiation.responseValue.find("server_no_context_takeover"), std::string::npos);
        EXPECT_NE(negotiation.responseValue.find("client_no_context_takeover"), std::string::npos);
    }

    /**
     * @brief 多个扩展并存时只挑出 permessage-deflate，参数也要能被跳过
     */
    TEST(PerMessageDeflate, FindsTheExtensionAmongOtherOffers)
    {
        EXPECT_TRUE(negotiatePerMessageDeflate("foo, permessage-deflate; client_max_window_bits, bar").accepted);
        EXPECT_TRUE(negotiatePerMessageDeflate("permessage-deflate;server_max_window_bits=10").accepted);
        // 大小写不敏感：扩展名是 token 语义
        EXPECT_TRUE(negotiatePerMessageDeflate("PerMessage-Deflate").accepted);
    }

    /**
     * @brief 对端没提供该扩展时不接受，也不回任何取值
     */
    TEST(PerMessageDeflate, RejectsWhenNotOffered)
    {
        EXPECT_FALSE(negotiatePerMessageDeflate("").accepted);
        EXPECT_FALSE(negotiatePerMessageDeflate("foo, bar").accepted);
        EXPECT_FALSE(negotiatePerMessageDeflate("x-permessage-deflate").accepted) << "撞名的扩展不该被认成目标扩展";
        EXPECT_TRUE(negotiatePerMessageDeflate("foo, bar").responseValue.empty());
    }

    /**
     * @brief 压缩后再解压必须与原消息逐字节一致
     */
    TEST(PerMessageDeflate, RoundTripsMessages)
    {
        const std::string original = "permessage-deflate 往返：中文与 ASCII 混排，含空字节之前的普通文本";

        const std::optional<std::string> compressed = deflateWebSocketMessage(original);
        ASSERT_TRUE(compressed.has_value()) << "压缩失败";

        const std::optional<std::string> restored = inflateWebSocketMessage(*compressed, 64 * 1024);
        ASSERT_TRUE(restored.has_value()) << "解压失败";
        EXPECT_EQ(*restored, original);
    }

    /**
     * @brief 空消息也要能往返（线上负载是合法的压缩字节）
     */
    TEST(PerMessageDeflate, RoundTripsEmptyMessage)
    {
        const std::optional<std::string> compressed = deflateWebSocketMessage("");
        ASSERT_TRUE(compressed.has_value());

        const std::optional<std::string> restored = inflateWebSocketMessage(*compressed, 64);
        ASSERT_TRUE(restored.has_value());
        EXPECT_TRUE(restored->empty());
    }

    /**
     * @brief 不可压内容同样往返一致
     */
    TEST(PerMessageDeflate, RoundTripsIncompressiblePayload)
    {
        const std::string payload = makeIncompressiblePayload();

        const std::optional<std::string> compressed = deflateWebSocketMessage(payload);
        ASSERT_TRUE(compressed.has_value());

        const std::optional<std::string> restored = inflateWebSocketMessage(*compressed, payload.size() + 16);
        ASSERT_TRUE(restored.has_value());
        EXPECT_EQ(*restored, payload);
    }

    /**
     * @brief 重复内容确实变小：确认走的是真压缩
     */
    TEST(PerMessageDeflate, ShrinksRepetitivePayload)
    {
        const std::optional<std::string> compressed = deflateWebSocketMessage(kRepetitivePayload);
        ASSERT_TRUE(compressed.has_value());
        EXPECT_LT(compressed->size(), kRepetitivePayload.size() / 10);
        EXPECT_EQ(*inflateWebSocketMessage(*compressed, kRepetitivePayload.size()), kRepetitivePayload);
    }

    /**
     * @brief 解压结果超过上限即拒绝：压缩比可以被放大成 zip bomb
     */
    TEST(PerMessageDeflate, RefusesToInflateBeyondTheOutputLimit)
    {
        const std::optional<std::string> compressed = deflateWebSocketMessage(kRepetitivePayload);
        ASSERT_TRUE(compressed.has_value());

        // 上限比原消息小一个字节：必须判超限而不是把内容全部解出来再截断
        EXPECT_FALSE(inflateWebSocketMessage(*compressed, kRepetitivePayload.size() - 1).has_value())
                << "解压输出超过上限却仍然返回了内容";
        // 给足上限就正常
        EXPECT_TRUE(inflateWebSocketMessage(*compressed, kRepetitivePayload.size()).has_value());
    }

    /**
     * @brief 非法字节被拒绝：把随机字节当压缩负载传给解压器
     */
    TEST(PerMessageDeflate, RejectsInvalidCompressedBytes)
    {
        const std::string garbage = makeIncompressiblePayload();
        EXPECT_FALSE(inflateWebSocketMessage(garbage, 1024 * 1024).has_value()) << "随机字节被当成了合法压缩流";
    }
} // namespace AsynGyanis::Net
