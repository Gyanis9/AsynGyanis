/**
 * @file TestHttp3Session.cpp
 * @brief HTTP/3 会话层的用例：本端单向流的绑定与 SETTINGS 的产出
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 这几条用例只驱动会话本身——单向流的开流口与流数据出口都是测试给的假实现，
 *          因此不涉及 ngtcp2 与真实 UDP。被验证的是会话对 nghttp3 的绑定是否正确：
 *          控制流与两条 QPACK 流有没有按 RFC 9114 的规矩开出来、SETTINGS 有没有真的产在控制流上。
 */

#include "Net/Http3/Http3Session.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 本端（服务端）发起的单向流号序列：RFC 9000 §2.1 规定服务端发起的单向流号 ≡ 3 (mod 4)
        constexpr std::int64_t kFirstServerUnidirectionalStreamId = 3;
        constexpr std::int64_t kUnidirectionalStreamIdStep       = 4;

        /// HTTP/3 的单向流类型：控制流是 0（RFC 9114 §6.2.1）
        constexpr std::uint8_t kControlStreamType = 0x00;

        /// SETTINGS 帧的帧类型（RFC 9114 §7.2.4）
        constexpr std::uint8_t kSettingsFrameType = 0x04;

        /// 记下会话交给出口的一段流数据
        struct CapturedStreamData
        {
            std::int64_t              streamId{0};    ///< 流号
            std::vector<std::uint8_t> bytes;          ///< 字节
            bool                      isEndStream{false}; ///< 是否收尾
        };

        /**
         * @brief 按「服务端单向流号依次递增」给出流号的假开流口
         */
        class FakeStreamOpener
        {
        public:
            FakeStreamOpener() = default;

            /// 给出一条新的本端单向流号
            [[nodiscard]] std::int64_t operator()()
            {
                const std::int64_t streamId = m_nextStreamId;
                m_nextStreamId += kUnidirectionalStreamIdStep;
                m_openedStreamIds.push_back(streamId);
                return streamId;
            }

            /// 已经被开出来的流号（按开流顺序）
            [[nodiscard]] const std::vector<std::int64_t> &openedStreamIds() const noexcept
            {
                return m_openedStreamIds;
            }

        private:
            std::int64_t              m_nextStreamId{kFirstServerUnidirectionalStreamId}; ///< 下一条流号
            std::vector<std::int64_t> m_openedStreamIds;                                 ///< 已开出的流号
        };
    } // namespace

    /**
     * @brief 会话建立时开三条本端单向流并在控制流上产出 SETTINGS
     */
    TEST(Http3Session, BindsControlAndQpackStreamsThenEmitsSettings)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;

        // 会话按值存开流口，要观察它到底开了哪些流号就得把本对象按引用交进去（否则填的是副本）
        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(
                                         CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                             });

        ASSERT_TRUE(session.isUsable()) << "三条本端单向流都开得出来，会话却不是可用状态";
        ASSERT_EQ(opener.openedStreamIds().size(), 3U)
                << "会话应当正好开三条本端单向流（控制流 + QPACK 编码流 + QPACK 解码流）";

        session.flushPendingStreamData();

        ASSERT_FALSE(sentStreamData.empty()) << "会话建好后一段字节都没产出：SETTINGS 没有发出去";
        const CapturedStreamData &settingsFrame = sentStreamData.front();
        EXPECT_EQ(settingsFrame.streamId, opener.openedStreamIds().front())
                << "SETTINGS 应当产在控制流上（也就是第一条开出来的单向流）";
        ASSERT_GE(settingsFrame.bytes.size(), 2U) << "控制流上第一段字节太短，装不下流类型与帧类型";
        EXPECT_EQ(settingsFrame.bytes[0], kControlStreamType) << "单向流的第一字节应当是流类型，控制流为 0";
        EXPECT_EQ(settingsFrame.bytes[1], kSettingsFrameType) << "控制流上的第一个帧应当是 SETTINGS";
    }

    /**
     * @brief 吃下对端控制流上的 SETTINGS 之后会话仍可用
     */
    TEST(Http3Session, ConsumesPeerSettingsAndStaysUsable)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;

        // 会话按值存开流口，要观察它到底开了哪些流号就得把本对象按引用交进去（否则填的是副本）
        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(
                                         CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                             });
        ASSERT_TRUE(session.isUsable());
        session.flushPendingStreamData();

        // 客户端发起的单向流号 ≡ 2 (mod 4)，第一条就是控制流 2：内容是流类型 0x00 + 长度为 0 的 SETTINGS
        const std::vector<std::uint8_t> peerControlStreamBytes{kControlStreamType, kSettingsFrameType, 0x00};
        session.onStreamData(2, peerControlStreamBytes, false);

        EXPECT_FALSE(session.isBroken()) << "吃下对端合法的 SETTINGS 不该把会话弄坏";
        EXPECT_TRUE(session.isUsable());
    }

    /**
     * @brief 开不出本端单向流时会话如实不可用，且不会往出口写任何字节
     */
    TEST(Http3Session, IsUnusableAndSilentWhenNoUnidirectionalStreamCanBeOpened)
    {
        std::vector<CapturedStreamData> sentStreamData;

        Http3Session session([] { return std::int64_t{-1}; },
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(
                                         CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                             });

        EXPECT_FALSE(session.isUsable()) << "单向流都开不出来，会话不该报可用";
        session.flushPendingStreamData();
        EXPECT_TRUE(sentStreamData.empty()) << "会话不可用时不该往出口写任何字节";
    }
} // namespace AsynGyanis::Net
