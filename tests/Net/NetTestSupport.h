/**
 * @file NetTestSupport.h
 * @brief Net 模块单元测试共用的文本与字节小工具
 * @author Gyanis
 * @date 2026-09-18
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 报文类用例遍布 Http / Http2 / WebSocket 三个子目录，同一批「查子串 / 数出现次数 /
 *          拼字节串 / 判请求标识」的小工具曾各自复制多份；统一放本头供各测试文件包含。
 *          与 CoreTestSupport.h 的协程夹具、HttpTestSupport.h 的服务器夹具各司其职。
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net::TestSupport
{
    /// 自动生成的请求标识长度：4 位前缀 + '-' + 16 位十六进制
    inline constexpr std::size_t kGeneratedRequestIdLength = 4 + 1 + 16;

    /// 分隔符 '-' 在请求标识里的下标
    inline constexpr std::size_t kGeneratedRequestIdSeparatorIndex = 4;

    /**
     * @brief 判断文本里是否出现指定子串
     * @param haystack 待搜索文本
     * @param needle 目标子串
     * @return true 命中
     */
    [[nodiscard]] inline bool containsText(const std::string_view haystack, const std::string_view needle)
    {
        return haystack.find(needle) != std::string_view::npos;
    }

    /**
     * @brief 统计文本里指定子串出现的次数
     * @param text 待搜索文本
     * @param needle 目标子串
     * @return std::size_t 出现次数
     */
    [[nodiscard]] inline std::size_t countTextOccurrences(const std::string_view text, const std::string_view needle)
    {
        std::size_t occurrenceCount = 0;
        for (std::size_t foundPosition = text.find(needle); foundPosition != std::string_view::npos; foundPosition = text.find(needle, foundPosition + needle.size()))
        {
            ++occurrenceCount;
        }
        return occurrenceCount;
    }

    /**
     * @brief 由字节序列拼出二进制文本
     * @details 不能直接用字符串字面量：帧头与负载里常含 0x00，按 const char* 构造会被零终止截断，
     *          那样断言就测不到完整字节了。
     * @param byteValues 字节值序列
     * @return std::string 逐字节写入的结果
     */
    [[nodiscard]] inline std::string makeBytes(const std::initializer_list<unsigned char> byteValues)
    {
        std::string bytes;
        bytes.reserve(byteValues.size());
        for (const unsigned char byteValue: byteValues)
        {
            bytes.push_back(static_cast<char>(byteValue));
        }
        return bytes;
    }

    /**
     * @brief 把 32 位无符号数按大端写成 4 字节
     * @param value 待写出的数值
     * @return std::string 4 字节
     */
    [[nodiscard]] inline std::string makeBigEndian32(const std::uint32_t value)
    {
        std::string bytes;
        for (int shiftBitCount = 24; shiftBitCount >= 0; shiftBitCount -= 8)
        {
            bytes.push_back(static_cast<char>((value >> shiftBitCount) & 0xFFU));
        }
        return bytes;
    }

    /**
     * @brief 由字节序列拼出可直接喂给 span 接口的无符号字节容器
     * @details QUIC 的报文层按「指针 + 长度」收 `std::uint8_t`，用字符串字面量拼既不直观
     *          也躲不开零终止，因此与 makeBytes 各留一份：那份给字符串口，这份给无符号字节口。
     * @param byteValues 字节值序列
     * @return std::vector<std::uint8_t> 逐字节拷入的结果
     */
    [[nodiscard]] inline std::vector<std::uint8_t> makeUnsignedBytes(const std::initializer_list<unsigned char> byteValues)
    {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(byteValues.size());
        for (const unsigned char byteValue: byteValues)
        {
            bytes.push_back(byteValue);
        }
        return bytes;
    }

    /**
     * @brief 把十六进制文本解析成字节序列，空白与非十六进制字符一律跳过
     * @details 网络协议的官方测试向量都是按 RFC 的排版给的十六进制串（每行 16 字节、行内带空格），
     *          逐字节改写成 initializer_list 既容易抄错也无法跟原文对照，所以这里直接贴原文。
     * @param hexadecimalText 十六进制文本，大小写与空白不限
     * @return std::vector<std::uint8_t> 解析出的字节；末尾落单的半个字节被忽略
     */
    [[nodiscard]] inline std::vector<std::uint8_t> makeBytesFromHex(std::string_view hexadecimalText)
    {
        auto digitValue = [](const char character) -> int
        {
            if (character >= '0' && character <= '9')
            {
                return character - '0';
            }
            if (character >= 'a' && character <= 'f')
            {
                return character - 'a' + 10;
            }
            if (character >= 'A' && character <= 'F')
            {
                return character - 'A' + 10;
            }
            return -1;
        };

        std::vector<std::uint8_t> bytes;
        bytes.reserve(hexadecimalText.size() / 2 + 1);
        int pendingHighNibble = -1;
        for (const char character: hexadecimalText)
        {
            const int value = digitValue(character);
            if (value < 0)
            {
                continue;
            }
            if (pendingHighNibble < 0)
            {
                pendingHighNibble = value;
                continue;
            }
            bytes.push_back(static_cast<std::uint8_t>((pendingHighNibble << 4) | value));
            pendingHighNibble = -1;
        }
        return bytes;
    }

    /**
     * @brief 把按字节存的 std::string 转成无符号字节容器
     * @details 编码器一律往 `std::string` 里追加（二进制安全），而断言的期望值多来自 RFC 的
     *          十六进制向量，两边类型不同没法直接比；`char` 与 `std::uint8_t` 也不能用
     *          初始化列表隐式互转，所以显式过一层。
     * @param bytes 按字节存的内容
     * @return std::vector<std::uint8_t> 逐字节拷出的结果
     */
    [[nodiscard]] inline std::vector<std::uint8_t> toUnsignedBytes(const std::string &bytes)
    {
        std::vector<std::uint8_t> converted;
        converted.reserve(bytes.size());
        for (const char byteValue: bytes)
        {
            converted.push_back(static_cast<std::uint8_t>(byteValue));
        }
        return converted;
    }

    /**
     * @brief 判断请求标识是否符合自动生成格式
     * @param requestId 待判定的标识
     * @return true 长度符合约定，且分隔符位置与其余字符都是小写十六进制
     */
    [[nodiscard]] inline bool looksLikeGeneratedRequestId(const std::string_view requestId)
    {
        if (requestId.size() != kGeneratedRequestIdLength || requestId[kGeneratedRequestIdSeparatorIndex] != '-')
        {
            return false;
        }

        for (std::size_t index = 0; index < requestId.size(); ++index)
        {
            if (index == kGeneratedRequestIdSeparatorIndex)
            {
                continue;
            }
            const char character       = requestId[index];
            const bool isLowerHexDigit = (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
            if (!isLowerHexDigit)
            {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 用给定的头部行拼出一条完整 GET 报文
     * @param headerLines 头部行原文，不含行尾 CRLF
     * @return std::string 可直接喂给 parse() 的报文
     */
    [[nodiscard]] inline std::string makeRequestTextWithHeaders(const std::vector<std::string> &headerLines)
    {
        std::string message = "GET /indexed HTTP/1.1\r\n";
        for (const std::string &headerLine: headerLines)
        {
            message.append(headerLine);
            message.append("\r\n");
        }
        message.append("\r\n");
        return message;
    }
    /**
     * @brief 临时文件夹具：给「映射正文」的用例提供一份磁盘上的真实文件
     *
     * @details 文件内容按二进制写入（文本模式会在 Windows 上把换行翻译成 CRLF，
     *          正文长度随之失真）；析构时递归删除整个临时目录，失败退出也能清理干净。
     */
    class TemporaryFile
    {
    public:
        /**
         * @brief 创建临时目录并写入待映射的文件（文件名为 body.bin）
         * @param namePrefix 便于调试的用途前缀
         * @param content 文件内容
         */
        TemporaryFile(const std::string &namePrefix, const std::string_view content)
        {
            static std::atomic<unsigned int> sequenceCounter{0};

            const std::string salt = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(sequenceCounter.fetch_add(1));
            m_directory            = std::filesystem::temp_directory_path() / ("AsynGyanis_Net_" + namePrefix + "_" + salt);

            std::error_code error;
            std::filesystem::create_directories(m_directory, error);
            m_filePath = m_directory / "body.bin";

            std::ofstream file(m_filePath, std::ios::out | std::ios::binary | std::ios::trunc);
            if (file.is_open())
            {
                file.write(content.data(), static_cast<std::streamsize>(content.size()));
            }
        }

        ~TemporaryFile()
        {
            std::error_code error;
            std::filesystem::remove_all(m_directory, error);
        }

        TemporaryFile(const TemporaryFile &) = delete;

        TemporaryFile &operator=(const TemporaryFile &) = delete;

        /**
         * @brief 文件路径
         * @return const std::filesystem::path& 映射用的文件绝对路径
         */
        [[nodiscard]] const std::filesystem::path &path() const noexcept
        {
            return m_filePath;
        }

    private:
        std::filesystem::path m_directory; ///< 本次用例独占的临时目录
        std::filesystem::path m_filePath;  ///< 目录内待映射的文件
    };
} // namespace AsynGyanis::Net::TestSupport
