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

    /**
     * @brief 钉住：MiB 级、但没到 int 上限的文本仍然按字节与字符数完整往返
     * @details 转换层新加了一道「长度超过 int 上限即按失败返回空串」的判界（Windows 那两个 API 的
     *          负长度不是长度，而是「自行扫到 NUL 为止」的哨兵，会把调用方的缓冲区读穿）。这条用例
     *          钉住那道判界没有把合法的大输入一起挡掉，也没有把字节数与字符数混为一谈：真要构造
     *          越过 2 GiB 的输入才能命中拒绝支路，那种内存量在 CI 上不现实。
     */
    TEST(TextEncoding, MultiMiBTextStillRoundTripsIntact)
    {
        constexpr std::size_t kRepeatCount = 1024U * 1024U;

        std::string utf8Text;
        utf8Text.reserve(kRepeatCount * 4U);
        for (std::size_t index = 0; index < kRepeatCount; ++index)
        {
            // 一段 1 字节 + 一段 3 字节：字节数与字符数因此不相等，判界写错对象会当场露出来
            utf8Text.push_back('a');
            utf8Text.append("\xE5\xA3\xAB");
        }
        ASSERT_EQ(utf8Text.size(), kRepeatCount * 4U);

        const std::wstring wideText = TextEncoding::toWideString(utf8Text);
        ASSERT_EQ(wideText.size(), kRepeatCount * 2U) << "宽字符数不对：判界或转换把字节数当成了字符数";

        EXPECT_EQ(TextEncoding::toUtf8String(wideText), utf8Text) << "MiB 级文本往返不一致";
    }
} // namespace AsynGyanis::Platform
