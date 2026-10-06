// HttpStreamBody 单元测试：交付语义与「消费才归还接收窗口」的记账
#include "Net/Http/HttpStreamBody.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
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
            [[nodiscard]] HttpStreamBody::ConsumeHandler makeHandler()
            {
                return [this](const std::size_t consumedByteCount) { reports.push_back(consumedByteCount); };
            }
        };
    } // namespace

    /**
     * @brief 追加不归还窗口，交付之后才归还——背压记账的核心契约
     */
    TEST(HttpStreamBody, CreditsOnlyAfterDelivery)
    {
        ConsumeRecord         record;
        HttpStreamBody        body;
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
    TEST(HttpStreamBody, CreditsFlowControlByteCountIncludingPadding)
    {
        ConsumeRecord  record;
        HttpStreamBody body;
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
    TEST(HttpStreamBody, CreditsZeroLengthFrameWithPadding)
    {
        ConsumeRecord  record;
        HttpStreamBody body;
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
    TEST(HttpStreamBody, DeliversAndCreditsAccumulatedBytes)
    {
        ConsumeRecord  record;
        HttpStreamBody body;
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
    TEST(HttpStreamBody, ConsumePendingCreditsEverythingOutstanding)
    {
        ConsumeRecord  record;
        HttpStreamBody body;
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
    TEST(HttpStreamBody, BodyTooLargeMarksBrokenAndIsReported)
    {
        ConsumeRecord  record;
        HttpStreamBody body;
        body.reset(record.makeHandler());

        body.append(std::string(16, 'a'), 16, false);
        body.markBodyTooLarge();

        EXPECT_TRUE(body.isBodyTooLarge());
        EXPECT_TRUE(body.isBroken()) << "越界之后正文读不下去，处理器应当立刻看到流终止";
    }

    /**
     * @brief 对端取消：终止流但不改「越界」标记
     */
    TEST(HttpStreamBody, BrokenIsReportedWithoutTooLargeFlag)
    {
        HttpStreamBody body;
        body.reset({});

        body.markBroken();

        EXPECT_TRUE(body.isBroken());
        EXPECT_FALSE(body.isBodyTooLarge());
    }

    /**
     * @brief reset() 换流时清空内容与全部标记，并替换消费回调
     */
    TEST(HttpStreamBody, ResetClearsContentAndMarkers)
    {
        ConsumeRecord  firstRecord;
        ConsumeRecord  secondRecord;
        HttpStreamBody body;
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
    TEST(HttpStreamBody, DiscardWithoutConsumeHandlerIsSafe)
    {
        HttpStreamBody body;
        body.reset({});

        body.append(std::string(32, 'a'), 32, false);
        body.discardBufferedBody();

        EXPECT_TRUE(body.bufferedBodyView().empty());
    }

    /**
     * @brief 正文不经请求对象中转：收齐后没有「残余」可交（completedBody 恒为空）
     */
    TEST(HttpStreamBody, CompletedBodyIsAlwaysEmpty)
    {
        HttpStreamBody body;
        body.reset({});

        body.append(std::string(16, 'a'), 16, true);

        EXPECT_TRUE(body.isComplete());
        EXPECT_TRUE(body.completedBody().empty()) << "h2 的残余应当经 bufferedBodyView() 交付，completedBody() 恒为空";
    }

    /**
     * @brief 声明 13 字节却只来了 5 字节就 END_STREAM：来源同时是「已收尾」与「没发满」
     * @details RFC 9113 §8.1.1 判这一形状畸形。isComplete() 在这一格仍为真（收尾信号确实到了），
     *          所以截断必须由 isTruncated() 单独答得出，否则业务只看到一次正常 EOF
     */
    TEST(HttpStreamBody, ShortEndStreamAgainstDeclaredLengthIsTruncated)
    {
        HttpStreamBody body;
        body.reset({});
        body.setDeclaredBodyByteCount(std::optional<std::size_t>{13});

        body.append(std::string(5, 'a'), 5, true);

        EXPECT_TRUE(body.isComplete()) << "对端确实收尾了：这一格不能被改成「没收尾」，否则读取方会再去泵一次等不来的字节";
        EXPECT_TRUE(body.isTruncated()) << "实收 5 不足声明的 13，业务问不出截断就是把半份正文当完整交出";
        EXPECT_TRUE(body.hasBodyLengthMismatch());
    }

    /**
     * @brief 收满声明的那一格：两条判据都不该响
     */
    TEST(HttpStreamBody, EndStreamWithExactlyTheDeclaredLengthIsNotTruncated)
    {
        HttpStreamBody body;
        body.reset({});
        body.setDeclaredBodyByteCount(std::optional<std::size_t>{5});

        body.append(std::string(5, 'a'), 5, true);

        EXPECT_TRUE(body.isComplete());
        EXPECT_FALSE(body.isTruncated()) << "收齐了却报截断，处理器会把完整的上传拒掉";
        EXPECT_FALSE(body.hasBodyLengthMismatch());
    }

    /**
     * @brief 多发（实收 > 声明）：算「与声明不符」，但不算「截断」
     * @details 业务侧的判据问的是「我拿到的正文齐不齐」，多出来的零头不影响答案的完整性；
     *          会话的运维日志要的才是「与声明不符」，两者各有一句
     */
    TEST(HttpStreamBody, OverDeclaredEndStreamIsMismatchButNotTruncated)
    {
        HttpStreamBody body;
        body.reset({});
        body.setDeclaredBodyByteCount(std::optional<std::size_t>{3});

        body.append(std::string(7, 'a'), 7, true);

        EXPECT_TRUE(body.hasBodyLengthMismatch()) << "多发同样是畸形请求，运维侧要看得见";
        EXPECT_FALSE(body.isTruncated()) << "字节并不缺，报截断会把「我拿到的是全的」这件事说反";
    }

    /**
     * @brief 没有可用声明值时判不出截断：两条判据都保持沉默，而不是猜一个
     * @details 会话对 HEAD 与非法 content-length 都不交代声明值。这时谎报截断会把正常请求拒掉，
     *          而「没声明」本来就没有可比的量
     */
    TEST(HttpStreamBody, WithoutDeclaredLengthNothingCanBeJudged)
    {
        HttpStreamBody body;
        body.reset({});

        body.append(std::string(5, 'a'), 5, true);

        EXPECT_TRUE(body.isComplete());
        EXPECT_FALSE(body.isTruncated());
        EXPECT_FALSE(body.hasBodyLengthMismatch());
    }

    /**
     * @brief 还没收尾就比长度：字节差额是「进行中」，不是截断
     */
    TEST(HttpStreamBody, PartialBodyStillInFlightIsNotTruncated)
    {
        HttpStreamBody body;
        body.reset({});
        body.setDeclaredBodyByteCount(std::optional<std::size_t>{13});

        body.append(std::string(5, 'a'), 5, false);

        EXPECT_FALSE(body.isComplete()) << "END_STREAM 没到，正文还在收";
        EXPECT_FALSE(body.isTruncated()) << "把进行中的差额报成截断，处理器会在第一批就掐死一条正常上传";
        EXPECT_FALSE(body.hasBodyLengthMismatch());
    }

    /**
     * @brief 声明值不跨流残留：reset() 之后下一条流没重新交代就判不出
     * @details 与 isComplete()/isBroken() 同一族：这份缓冲在同一条连接上是复用的
     */
    TEST(HttpStreamBody, DeclaredLengthDoesNotCrossStreams)
    {
        HttpStreamBody body;
        body.reset({});
        body.setDeclaredBodyByteCount(std::optional<std::size_t>{13});
        body.append(std::string(5, 'a'), 5, true);
        ASSERT_TRUE(body.isTruncated()) << "前提：上一条流确实被判成了截断";

        body.reset({});
        body.append(std::string(5, 'b'), 5, true);

        EXPECT_FALSE(body.isTruncated()) << "上一条流的 content-length 串到了这一条流上";
        EXPECT_FALSE(body.hasBodyLengthMismatch());
    }
} // namespace AsynGyanis::Net
