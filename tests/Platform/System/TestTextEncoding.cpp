// TextEncoding 单元测试：UTF-8 与 UTF-16 互转的正确性与异常输入
#include "Platform/Platform.h"
#include "Platform/System/TextEncoding.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace AsynGyanis::Platform
{
    namespace
    {
        /**
         * @brief 去掉转换补出的替换字符，只留下「原文里本来就在的内容」
         * @details 两个平台补 U+FFFD 的个数不同（Windows 按坏字节数补，本端按坏序列数补），这个差异
         *          不影响正确性；会咬人的是**正文少了一段**，所以按正文比对而不是按总长度比对。
         * @param wideText 转换结果
         * @return std::wstring 去掉 U+FFFD 后的结果
         */
        std::wstring dropReplacementCharacters(const std::wstring &wideText)
        {
            std::wstring survivors;
            for (const wchar_t codeUnit: wideText)
            {
                if (static_cast<std::uint32_t>(codeUnit) != 0xFFFDU)
                {
                    survivors.push_back(codeUnit);
                }
            }
            return survivors;
        }
    } // namespace

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

    /**
     * @brief 序列中途撞到不合法字节时，它身后的合法内容不得被一起跳过
     * @details 一个坏字节混进路径就会少一截字符，且没有任何失败信号——静默变形比报错难查得多。
     *          预期取 Windows 侧 `MultiByteToWideChar` 的实测行为：它在坏字节处重新同步，两个 '-'
     *          与 'x' 都活着出来。两侧补出的替换字符个数可以不同，故只核对留下来的正文。
     */
    TEST(TextEncoding, InvalidContinuationByteDoesNotSwallowFollowingText)
    {
        // 0xE4 声明三字节序列，紧跟的两个 '-' 都不是续字节
        const std::string leadFollowedByAscii("\xE4--b", 4);
        EXPECT_EQ(dropReplacementCharacters(TextEncoding::toWideString(leadFollowedByAscii)), L"--b");

        // 前一个续字节合法、'x' 不合法：'x' 要当成新字符的首字节重新解，而不是当成本序列的尾巴吃掉
        const std::string partiallyValidSequence("\xE4\xB8" "x", 3);
        EXPECT_EQ(dropReplacementCharacters(TextEncoding::toWideString(partiallyValidSequence)), L"x");
    }

    /**
     * @brief 孤立首字节后面那整个合法字符必须活着出来
     * @details 比上一条更狠的形状：坏字节后面紧跟一个完整的三字节汉字。按声明长度跳过会连带吃掉
     *          那个汉字的前两字节，结果里只剩替换字符——正文一个字符没少，却整字被吞。
     */
    TEST(TextEncoding, BadLeadByteDoesNotSwallowTheFollowingWholeCharacter)
    {
        const std::string badThenWholeCharacter("\xE4" "\xE5\xA3\xAB", 4);

        const std::wstring wideText = TextEncoding::toWideString(badThenWholeCharacter);

        const auto firstSurvivor = wideText.find_first_not_of(static_cast<wchar_t>(0xFFFD));
        ASSERT_NE(firstSurvivor, std::wstring::npos) << "结果里只剩替换字符，说明「士」被连带跳过了";
        EXPECT_EQ(static_cast<std::uint32_t>(wideText[firstSurvivor]), 0x58EBU);
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
