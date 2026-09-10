#include "Platform/System/TextEncoding.h"

#include <cstdint>

namespace AsynGyanis::Platform
{
    namespace
    {
#if ASYN_PLATFORM_LINUX
        /// UTF-8 编码相关的 Unicode 常量
        constexpr std::uint32_t kUnicodeReplacementCharacter = 0xFFFD;
        constexpr std::uint32_t kMaximumCodePoint            = 0x10FFFF;

        /**
         * @brief 把一个 Unicode 码位按 UTF-8 追加到输出缓冲
         * @param output 目标字节串
         * @param codePoint 待编码的码位
         */
        void appendUtf8(std::string &output, const std::uint32_t codePoint)
        {
            if (codePoint < 0x80)
            {
                output.push_back(static_cast<char>(codePoint));
            } else if (codePoint < 0x800)
            {
                output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
                output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            } else if (codePoint < 0x10000)
            {
                output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
                output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            } else
            {
                output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
                output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
                output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
        }
#endif
    } // namespace

    std::wstring TextEncoding::toWideString(const std::string &utf8Text)
    {
        if (utf8Text.empty())
        {
            return {};
        }

#if ASYN_PLATFORM_WIN32
        const int wideLength = ::MultiByteToWideChar(CP_UTF8, 0, utf8Text.c_str(), static_cast<int>(utf8Text.size()), nullptr, 0);
        if (wideLength <= 0)
        {
            return {};
        }
        std::wstring wideText(static_cast<std::size_t>(wideLength), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, utf8Text.c_str(), static_cast<int>(utf8Text.size()), wideText.data(), wideLength);
        return wideText;
#else
        // Linux 的 wchar_t 为 32 位，直接承载码位；不走 mbstowcs 是因为其结果依赖
        // 当前 C locale，进程未 setlocale 时会把 UTF-8 多字节序列判为非法。
        std::wstring wideText;
        wideText.reserve(utf8Text.size());

        for (std::size_t index = 0; index < utf8Text.size();)
        {
            const auto leadByte = static_cast<std::uint8_t>(utf8Text[index]);

            std::uint32_t codePoint    = 0;
            std::size_t   sequenceSize = 0;
            if (leadByte < 0x80)
            {
                codePoint    = leadByte;
                sequenceSize = 1;
            } else if ((leadByte & 0xE0) == 0xC0)
            {
                codePoint    = leadByte & 0x1FU;
                sequenceSize = 2;
            } else if ((leadByte & 0xF0) == 0xE0)
            {
                codePoint    = leadByte & 0x0FU;
                sequenceSize = 3;
            } else if ((leadByte & 0xF8) == 0xF0)
            {
                codePoint    = leadByte & 0x07U;
                sequenceSize = 4;
            } else
            {
                // 非法首字节（孤立的续段或 0xF8/0xFE/0xFF），以替换字符推进一字节
                wideText.push_back(static_cast<wchar_t>(kUnicodeReplacementCharacter));
                ++index;
                continue;
            }

            if (index + sequenceSize > utf8Text.size())
            {
                wideText.push_back(static_cast<wchar_t>(kUnicodeReplacementCharacter));
                break;
            }

            bool isSequenceValid = true;
            for (std::size_t offset = 1; offset < sequenceSize; ++offset)
            {
                const auto continuationByte = static_cast<std::uint8_t>(utf8Text[index + offset]);
                if ((continuationByte & 0xC0) != 0x80)
                {
                    isSequenceValid = false;
                    break;
                }
                codePoint = (codePoint << 6) | (continuationByte & 0x3FU);
            }

            index += sequenceSize;

            // 拒绝过长编码、代理区码位与超出 Unicode 范围的码位
            const bool isOverlong  = (sequenceSize == 2 && codePoint < 0x80) || (sequenceSize == 3 && codePoint < 0x800) || (sequenceSize == 4 && codePoint < 0x10000);
            const bool isSurrogate = codePoint >= 0xD800 && codePoint <= 0xDFFF;
            if (!isSequenceValid || isOverlong || isSurrogate || codePoint > kMaximumCodePoint)
            {
                wideText.push_back(static_cast<wchar_t>(kUnicodeReplacementCharacter));
                continue;
            }

            wideText.push_back(static_cast<wchar_t>(codePoint));
        }

        return wideText;
#endif
    }

    std::string TextEncoding::toUtf8String(const std::wstring &wideText)
    {
        if (wideText.empty())
        {
            return {};
        }

#if ASYN_PLATFORM_WIN32
        const int narrowLength = ::WideCharToMultiByte(CP_UTF8, 0, wideText.c_str(), static_cast<int>(wideText.size()), nullptr, 0, nullptr, nullptr);
        if (narrowLength <= 0)
        {
            return {};
        }
        std::string utf8Text(static_cast<std::size_t>(narrowLength), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wideText.c_str(), static_cast<int>(wideText.size()), utf8Text.data(), narrowLength, nullptr, nullptr);
        return utf8Text;
#else
        std::string utf8Text;
        utf8Text.reserve(wideText.size());

        for (std::size_t index = 0; index < wideText.size(); ++index)
        {
            const auto codeUnit = static_cast<std::uint32_t>(wideText[index]);

            if ((codeUnit >= 0xD800 && codeUnit <= 0xDFFF) || codeUnit > kMaximumCodePoint)
            {
                appendUtf8(utf8Text, kUnicodeReplacementCharacter);
                continue;
            }

            appendUtf8(utf8Text, codeUnit);
        }

        return utf8Text;
#endif
    }
} // namespace AsynGyanis::Platform
