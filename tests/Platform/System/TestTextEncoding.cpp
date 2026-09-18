// TextEncoding 单元测试：UTF-8 与 UTF-16 互转的正确性与异常输入
#include "Platform/Platform.h"
#include "Platform/System/TextEncoding.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace AsynGyanis::Platform
{
    TEST(TextEncoding, ToWideStringReturnsEmptyForEmptyInput)
    {
        EXPECT_TRUE(TextEncoding::toWideString(std::string()).empty());
    }

    TEST(TextEncoding, ToUtf8StringReturnsEmptyForEmptyInput)
    {
        EXPECT_TRUE(TextEncoding::toUtf8String(std::wstring()).empty());
    }

    TEST(TextEncoding, AsciiRoundTripKeepsTextIntact)
    {
        const std::string original = "C:\\Users\\Gyanis\\config.yaml";

        const std::wstring wideText   = TextEncoding::toWideString(original);
        const std::string  roundTrip  = TextEncoding::toUtf8String(wideText);

        EXPECT_EQ(wideText.size(), original.size());
        EXPECT_EQ(roundTrip, original);
    }

    TEST(TextEncoding, ChineseRoundTripKeepsCharacterCount)
    {
        const std::string original = "中文日志配置路径";

        const std::wstring wideText  = TextEncoding::toWideString(original);
        const std::string  roundTrip = TextEncoding::toUtf8String(wideText);

        // 每个汉字在宽字符侧占一个码位
        EXPECT_EQ(wideText.size(), 8U);
        EXPECT_EQ(roundTrip, original);
    }

    TEST(TextEncoding, MixedScriptRoundTripKeepsTextIntact)
    {
        const std::string original = "服务器 server-01 端口 8080";

        EXPECT_EQ(TextEncoding::toUtf8String(TextEncoding::toWideString(original)), original);
    }

    TEST(TextEncoding, EmojiRoundTripKeepsTextIntact)
    {
        // 4 字节 UTF-8 序列，需要 UTF-16 代理对承载
        const std::string original = "status=ok \U0001F680";

        const std::wstring wideText  = TextEncoding::toWideString(original);
        const std::string  roundTrip = TextEncoding::toUtf8String(wideText);

#if ASYN_PLATFORM_WIN32
        // Windows 的 wchar_t 为 16 位，火箭表情占两个代理单元
        EXPECT_EQ(wideText.size(), original.size() - 2U);
#else
        EXPECT_EQ(wideText.size(), original.size() - 3U);
#endif
        EXPECT_EQ(roundTrip, original);
    }

    TEST(TextEncoding, InvalidLeadByteBecomesReplacementCharacterOnLinux)
    {
        const std::string invalidSequence("\xFF\xFE", 2);

        const std::wstring wideText = TextEncoding::toWideString(invalidSequence);

#if ASYN_PLATFORM_LINUX
        // 自研解码器对非法首字节按 U+FFFD 逐字节推进，保证长度可预期
        EXPECT_EQ(wideText.size(), 2U);
        EXPECT_EQ(static_cast<std::uint32_t>(wideText[0]), 0xFFFDU);
#else
        // Windows 交由系统 API 判定，只要求不崩溃且不长于输入
        EXPECT_LE(wideText.size(), 4U);
#endif
    }

    TEST(TextEncoding, TruncatedMultiByteSequenceBecomesReplacementCharacter)
    {
        // 只留下一个 3 字节序列的首字节
        const std::string truncated("\xE4\xB8", 2);

        const std::wstring wideText = TextEncoding::toWideString(truncated);

#if ASYN_PLATFORM_LINUX
        EXPECT_EQ(wideText.size(), 1U);
        EXPECT_EQ(static_cast<std::uint32_t>(wideText[0]), 0xFFFDU);
#else
        EXPECT_TRUE(wideText.empty() || wideText.size() == 1U);
#endif
    }

    TEST(TextEncoding, LoneSurrogateCodeUnitBecomesReplacementCharacterOnLinux)
    {
        const std::wstring wideWithSurrogate{static_cast<wchar_t>(0xD800)};

        const std::string utf8Text = TextEncoding::toUtf8String(wideWithSurrogate);

#if ASYN_PLATFORM_LINUX
        // U+FFFD 的 UTF-8 编码固定为 3 字节
        EXPECT_EQ(utf8Text.size(), 3U);
#endif
        EXPECT_FALSE(utf8Text.empty());
    }
} // namespace AsynGyanis::Platform
