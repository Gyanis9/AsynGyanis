/**
 * @file TestHttp2StreamBody.cpp
 * @brief Http2StreamBody 单元测试：交付语义与「消费才归还接收窗口」的记账
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Net/Http2/Http2StreamBody.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 记录消费回调收到的每一次报量，便于逐次断言
         */
        struct ConsumeRecord
        {
            std::vector<std::size_t> reports; ///< 每次回调的入参（流控字节数）

            /// 造一个绑定到本记录的回调
            [[nodiscard]] Http2StreamBody::ConsumeHandler makeHandler()
            {
                return [this](const std::size_t consumedByteCount)
                {
                    reports.push_back(consumedByteCount);
                };
            }
        };
    } // namespace

    /**
     * @brief 追加不归还窗口，交付之后才归还——背压记账的核心契约
     */
    TEST(Http2StreamBody, CreditsOnlyAfterDelivery)
    {
        ConsumeRecord      record;
        Http2StreamBody    body;
        constexpr std::size_t kChunkBytes = 1000;
        body.reset(record.makeHandler());

        body.append(std::string(kChunkBytes, 'a'), kChunkBytes, false);

        EXPECT_TRUE(record.reports.empty()) << "字节刚到达就归还了窗口：背压没有落在消费上";
        ASSERT_EQ(body.bufferedBodyView().size(), kChunkBytes) << "追加的字节应当立即可交付";

        body.discardBufferedBody();

        ASSERT_EQ(record.reports.size(), 1U) << "交付之后应当归还一次窗口";
        EXPECT_EQ(record.reports.front(), kChunkBytes);
        EXPECT_TRUE(body.bufferedBodyView().empty()) << "交付过的字节应当被丢掉";

        // 没有新交付就没有新归还：重复丢弃不该再报一次
        body.discardBufferedBody();
        EXPECT_EQ(record.reports.size(), 1U) << "没有新字节被丢弃却又归还了一次窗口，对端窗口会被重复放大";
    }

    /**
     * @brief 归还的量按 DATA 帧负载原长（含 padding）报，而不是按应用数据长度
     * @details padding 也占对端的发送窗口（RFC 9113 §6.9.1）：只按应用数据报量会让窗口被 padding 一点点吃掉
     */
    TEST(Http2StreamBody, CreditsFlowControlByteCountIncludingPadding)
    {
        ConsumeRecord   record;
        Http2StreamBody body;
        body.reset(record.makeHandler());

        // 应用数据 100 字节，而该帧占用 150 个流控字节（50 字节 padding）
        body.append(std::string(100, 'a'), 150, false);
        body.discardBufferedBody();

        ASSERT_EQ(record.reports.size(), 1U);
        EXPECT_EQ(record.reports.front(), 150U) << "应当按帧负载原长归还，padding 也算在内";
    }

    /**
     * @brief 零长 DATA 帧（常见于只带 END_STREAM 的收尾帧）同样要归还它占用的流控字节
     */
    TEST(Http2StreamBody, CreditsZeroLengthFrameWithPadding)
    {
        ConsumeRecord   record;
        Http2StreamBody body;
        body.reset(record.makeHandler());

        body.append({}, 8, true);
        body.discardBufferedBody();

        ASSERT_EQ(record.reports.size(), 1U) << "零长帧占用的窗口没有归还";
        EXPECT_EQ(record.reports.front(), 8U);
        EXPECT_TRUE(body.isComplete()) << "END_STREAM 应当把正文标成收齐";
    }

    /**
     * @brief 一次交付覆盖「两次追加」的全部字节：按到达批次攒起来一起交，归还量是两者之和
     */
    TEST(Http2StreamBody, DeliversAndCreditsAccumulatedBytes)
    {
        ConsumeRecord   record;
        Http2StreamBody body;
        body.reset(record.makeHandler());

        body.append(std::string(300, 'a'), 300, false);
        body.append(std::string(200, 'b'), 220, false);

        EXPECT_TRUE(record.reports.empty()) << "追加阶段不该归还窗口";
        EXPECT_EQ(body.bufferedBodyView().size(), 500U) << "两批应当攒成一段交付";
        EXPECT_EQ(body.totalReceivedByteCount(), 500U) << "累计收到的字节数按应用数据算";

        body.discardBufferedBody();
        ASSERT_EQ(record.reports.size(), 1U);
        EXPECT_EQ(record.reports.front(), 520U) << "归还量是两帧流控字节之和（300 + 220）";
    }

    /**
     * @brief consumePending()：业务不再读时把挂着的正文按已消费归还窗口（收尾路径依赖它）
     */
    TEST(Http2StreamBody, ConsumePendingCreditsEverythingOutstanding)
    {
        ConsumeRecord   record;
        Http2StreamBody body;
        body.reset(record.makeHandler());

        body.append(std::string(4096, 'a'), 4096, false);
        body.consumePending();

        ASSERT_EQ(record.reports.size(), 1U) << "收尾时没有把挂着的正文归还窗口，对端窗口会被占用到流结束";
        EXPECT_EQ(record.reports.front(), 4096U);
        EXPECT_TRUE(body.bufferedBodyView().empty());
    }

    /**
     * @brief 越界标记：既终止流，也让会话据此回 413
     */
    TEST(Http2StreamBody, BodyTooLargeMarksBrokenAndIsReported)
    {
        ConsumeRecord   record;
        Http2StreamBody body;
        body.reset(record.makeHandler());

        body.append(std::string(16, 'a'), 16, false);
        body.markBodyTooLarge();

        EXPECT_TRUE(body.isBodyTooLarge());
        EXPECT_TRUE(body.isBroken()) << "越界之后正文读不下去，处理器应当立刻看到流终止";
    }

    /**
     * @brief 对端取消：终止流但不改「越界」标记
     */
    TEST(Http2StreamBody, BrokenIsReportedWithoutTooLargeFlag)
    {
        Http2StreamBody body;
        body.reset({});

        body.markBroken();

        EXPECT_TRUE(body.isBroken());
        EXPECT_FALSE(body.isBodyTooLarge());
    }

    /**
     * @brief reset() 换流时清空内容与全部标记，并替换消费回调
     */
    TEST(Http2StreamBody, ResetClearsContentAndMarkers)
    {
        ConsumeRecord   firstRecord;
        ConsumeRecord   secondRecord;
        Http2StreamBody body;
        body.reset(firstRecord.makeHandler());

        body.append(std::string(64, 'a'), 64, true);
        body.markBodyTooLarge();
        body.reset(secondRecord.makeHandler());

        EXPECT_EQ(body.bufferedBodyView().size(), 0U);
        EXPECT_EQ(body.pendingByteCount(), 0U);
        EXPECT_EQ(body.totalReceivedByteCount(), 0U);
        EXPECT_FALSE(body.isComplete()) << "END_STREAM 标记不该跨流残留";
        EXPECT_FALSE(body.isBroken());
        EXPECT_FALSE(body.isBodyTooLarge());

        // 回调已换成新的那个：丢弃动作应当只报给新回调
        body.append(std::string(10, 'b'), 10, false);
        body.discardBufferedBody();
        EXPECT_TRUE(firstRecord.reports.empty()) << "旧回调仍在被调用";
        ASSERT_EQ(secondRecord.reports.size(), 1U);
        EXPECT_EQ(secondRecord.reports.front(), 10U);
    }

    /**
     * @brief 没有消费回调时丢弃字节不该崩——回调是可选装配
     */
    TEST(Http2StreamBody, DiscardWithoutConsumeHandlerIsSafe)
    {
        Http2StreamBody body;
        body.reset({});

        body.append(std::string(32, 'a'), 32, false);
        body.discardBufferedBody();

        EXPECT_TRUE(body.bufferedBodyView().empty());
    }

    /**
     * @brief 正文不经请求对象中转：收齐后没有「残余」可交（completedBody 恒为空）
     */
    TEST(Http2StreamBody, CompletedBodyIsAlwaysEmpty)
    {
        Http2StreamBody body;
        body.reset({});

        body.append(std::string(16, 'a'), 16, true);

        EXPECT_TRUE(body.isComplete());
        EXPECT_TRUE(body.completedBody().empty()) << "h2 的残余应当经 bufferedBodyView() 交付，completedBody() 恒为空";
    }
} // namespace AsynGyanis::Net
