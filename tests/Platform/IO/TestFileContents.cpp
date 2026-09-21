// FileContents 单元测试：整段读、按区间读、短读、越界偏移、空长度、缺失路径与二进制安全
#include "Platform/IO/FileContents.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>

#include "PlatformTestSupport.h"

namespace AsynGyanis::Platform
{
    namespace
    {
        /**
         * @brief 读一段字节并断言成功，把内容交回调用方
         * @param filePath 目标文件
         * @param offset 起始偏移，单位为字节
         * @param length 期望读出的字节数
         * @return std::string 读出的字节
         */
        [[nodiscard]] std::string readOrThrow(const std::filesystem::path &filePath,
                                              const std::size_t offset,
                                              const std::size_t length)
        {
            const std::expected<std::string, std::error_code> result = readFileContents(filePath, offset, length);
            EXPECT_TRUE(result.has_value()) << "读取失败：" << result.error().message();
            return result.has_value() ? *result : std::string{};
        }
    } // namespace

    /**
     * @brief 钉住：偏移 0 读满长度时，交回的正是文件的完整内容
     */
    TEST(FileContents, ReadsWholeFileFromZeroOffset)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileContents_Whole");
        ASSERT_TRUE(temporaryDirectory.writeFile("asset.bin", "hello-static-body"));

        const std::string contents = readOrThrow(temporaryDirectory.path() / "asset.bin", 0U, 17U);
        EXPECT_EQ(contents, "hello-static-body");
        EXPECT_EQ(contents.size(), 17U);
    }

    /**
     * @brief 钉住：按 (偏移, 长度) 只交出区间内的那段字节，不带头不带尾
     */
    TEST(FileContents, ReadsOnlyTheRequestedRange)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileContents_Range");
        ASSERT_TRUE(temporaryDirectory.writeFile("asset.bin", "0123456789abcdef"));

        const std::string contents = readOrThrow(temporaryDirectory.path() / "asset.bin", 4U, 6U);
        EXPECT_EQ(contents, "456789") << "区间读必须逐字节对齐偏移，多一位少一位都会让 206 的正文错位";
    }

    /**
     * @brief 钉住：要的长度越过文件末尾时以短读表达，而不是报错也不是补零
     * @details 调用方（静态文件服务）靠「实际长度 < 期望长度」认出「stat 之后文件被截断」，
     *          因此这里既不能失败也不能把缺的部分填出来。
     */
    TEST(FileContents, ReportsShortReadWhenLengthRunsPastTheEnd)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileContents_Short");
        ASSERT_TRUE(temporaryDirectory.writeFile("asset.bin", "abc"));

        const std::expected<std::string, std::error_code> result = readFileContents(temporaryDirectory.path() / "asset.bin",
                                                                                   0U, 128U);
        ASSERT_TRUE(result.has_value()) << "文件比期望的短不是错误，应当交回实际读到的那段";
        EXPECT_EQ(result->size(), 3U) << "短读的长度必须如实，不许补零凑数";
        EXPECT_EQ(*result, "abc");
    }

    /**
     * @brief 钉住：偏移落在文件末尾之外时读到空段，同样不算错误
     */
    TEST(FileContents, ReadsNothingWhenOffsetIsPastTheEnd)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileContents_PastEnd");
        ASSERT_TRUE(temporaryDirectory.writeFile("asset.bin", "abc"));

        const std::expected<std::string, std::error_code> result = readFileContents(temporaryDirectory.path() / "asset.bin",
                                                                                   999999U, 16U);
        ASSERT_TRUE(result.has_value()) << "越界偏移是「读到 0 字节」，不是失败";
        EXPECT_TRUE(result->empty());
    }

    /**
     * @brief 钉住：长度为 0 直接给出空串，连文件都不打开
     * @details 用一条不存在的路径来钉「不打开」这件事：真去打开它必然报错。
     */
    TEST(FileContents, SkipsOpeningTheFileForZeroLength)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileContents_ZeroLength");
        const std::filesystem::path           missingPath = temporaryDirectory.path() / "never-created.bin";

        const std::expected<std::string, std::error_code> result = readFileContents(missingPath, 0U, 0U);
        ASSERT_TRUE(result.has_value()) << "空段不需要知道文件在不在";
        EXPECT_TRUE(result->empty());
    }

    /**
     * @brief 钉住：路径不存在时以错误码表达，不抛也不给空串冒充成功
     */
    TEST(FileContents, YieldsErrorCodeForMissingPath)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileContents_Missing");
        const std::filesystem::path           missingPath = temporaryDirectory.path() / "no-such-file.bin";

        const std::expected<std::string, std::error_code> result = readFileContents(missingPath, 0U, 8U);
        ASSERT_FALSE(result.has_value()) << "打不开文件不能当成「读到了空内容」";
        EXPECT_EQ(result.error().category(), std::system_category());
    }

    /**
     * @brief 钉住：内嵌 '\\0' 与高位字节按长度原样交出，二进制安全
     */
    TEST(FileContents, KeepsEmbeddedNullBytesAndHighBytes)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileContents_Binary");
        const std::string                     payload("ab\0cd\xff\x7f", 7);
        ASSERT_TRUE(temporaryDirectory.writeFile("asset.bin", payload));

        const std::string contents = readOrThrow(temporaryDirectory.path() / "asset.bin", 1U, 5U);
        EXPECT_EQ(contents.size(), 5U);
        EXPECT_EQ(contents, payload.substr(1, 5)) << "按长度取字节，不得在任何位置当成字符串结尾";
    }

    /**
     * @brief 钉住：就地读取把调用方给的缓冲填满并回报实际字节数，不另外造一份正文
     */
    TEST(FileContents, FillsTheCallersBufferAndReportsTheByteCount)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileContents_IntoBuffer");
        ASSERT_TRUE(temporaryDirectory.writeFile("asset.bin", "0123456789"));

        std::string buffer;
        const std::expected<std::size_t, std::error_code> bytesRead =
                readFileContentsInto(temporaryDirectory.path() / "asset.bin", 2U, 5U, buffer);
        ASSERT_TRUE(bytesRead.has_value()) << "读取失败：" << bytesRead.error().message();
        EXPECT_EQ(*bytesRead, 5U);
        EXPECT_EQ(buffer.size(), 5U);
        EXPECT_EQ(buffer, "23456");
    }

    /**
     * @brief 钉住：短读要就地收缩缓冲长度，而不是留下带尾部脏数据的长缓冲
     * @details 调用方（静态文件服务）把缓冲当正文发出去，缓冲多一位就会多发一位；
     *          回报值与缓冲长度必须同时是实际读到的数。
     */
    TEST(FileContents, ShrinksTheBufferToTheBytesActuallyRead)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileContents_IntoShort");
        ASSERT_TRUE(temporaryDirectory.writeFile("asset.bin", "abc"));

        std::string buffer("预先占好的一大段内容xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", 40);
        const std::expected<std::size_t, std::error_code> bytesRead =
                readFileContentsInto(temporaryDirectory.path() / "asset.bin", 0U, 128U, buffer);
        ASSERT_TRUE(bytesRead.has_value());
        EXPECT_EQ(*bytesRead, 3U);
        EXPECT_EQ(buffer.size(), 3U) << "缓冲没收缩，多发出去的就是旧内容的尾巴";
        EXPECT_EQ(buffer, "abc");
    }

    /**
     * @brief 钉住：读失败时以错误码表达，调用方据返回值判断即可，不必看缓冲内容
     */
    TEST(FileContents, YieldsErrorCodeForIntoReadOfMissingPath)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileContents_IntoMissing");
        const std::filesystem::path           missingPath = temporaryDirectory.path() / "no-such-file.bin";

        std::string buffer;
        const std::expected<std::size_t, std::error_code> bytesRead =
                readFileContentsInto(missingPath, 0U, 8U, buffer);
        ASSERT_FALSE(bytesRead.has_value()) << "打不开文件不能当成「读到了 0 字节」";
        EXPECT_EQ(bytesRead.error().category(), std::system_category());
    }
}
