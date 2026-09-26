// FileContents 单元测试：整段读、按区间读、短读、越界偏移、空长度、缺失路径与二进制安全
#include "Platform/IO/FileContents.h"

#include <gtest/gtest.h>
#include "Platform/FileSystem/FileBasicInfo.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
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
        [[nodiscard]] std::string readOrThrow(const std::filesystem::path &filePath, const std::size_t offset, const std::size_t length)
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

        const std::expected<std::string, std::error_code> result = readFileContents(temporaryDirectory.path() / "asset.bin", 0U, 128U);
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

        const std::expected<std::string, std::error_code> result = readFileContents(temporaryDirectory.path() / "asset.bin", 999999U, 16U);
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
    /**
     * @brief 钉住：出参交回的是**真的读到的那个对象**，且取值与按路径查的那条同刻度
     * @details 两条路的取值一旦分叉（换算、取整方向或身份标记算法不同），调用方一比就永远得到
     *          「不是同一版」，静态文件会整批回 500。这条就是拿来挡住那种分叉的。
     */
    TEST(FileContents, ReportsIdentityOfTheFileItActuallyOpened)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileContents_OpenedIdentity");
        ASSERT_TRUE(temporaryDirectory.writeFile("asset.bin", "hello-static-body"));
        const std::filesystem::path targetPath = temporaryDirectory.path() / "asset.bin";

        const std::optional<FileBasicInfo> byPath = queryFileBasicInfo(targetPath);
        ASSERT_TRUE(byPath.has_value());

        std::string                                       contents;
        FileBasicInfo                                     opened;
        const std::expected<std::size_t, std::error_code> result = readFileContentsInto(targetPath, 0U, 17U, contents, &opened);

        ASSERT_TRUE(result.has_value()) << result.error().message();
        EXPECT_TRUE(opened.isRegularFile);
        EXPECT_EQ(opened.sizeBytes, byPath->sizeBytes) << "按句柄查与按路径查的大小不是同一刻度";
        EXPECT_EQ(opened.lastWriteSeconds, byPath->lastWriteSeconds) << "两条路的修改秒不是同一刻度（取整方向分叉）";
        EXPECT_EQ(opened.identityTag, byPath->identityTag) << "两条路的身份标记不是同一算法，调用方一比就永远不等";
    }

    /**
     * @brief 钉住：查过元数据之后路径被换掉时，出参交回的是新版本而不是先前那份
     * @details 静态服务先查元数据算出 ETag、再打开读正文，中间文件被原子替换（部署就是这个动作）时
     *          读到的长度可以完全「对得上」（新文件更长，我们只取旧长度那段前缀），短读判据抓不住它。
     *          唯一能认出「字节与验证器不是同一版」的就是这条身份比对，所以它必须报出差异来。
     */
    TEST(FileContents, OpenedIdentityFollowsTheObjectAndNotThePathWhenTheFileIsReplaced)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileContents_ReplacedBetweenQueries");
        ASSERT_TRUE(temporaryDirectory.writeFile("asset.bin", "abc"));
        const std::filesystem::path targetPath = temporaryDirectory.path() / "asset.bin";

        const std::optional<FileBasicInfo> advertised = queryFileBasicInfo(targetPath);
        ASSERT_TRUE(advertised.has_value());
        ASSERT_EQ(advertised->sizeBytes, 3U);

        // 请求处理到「打开正文」之间，发布方把文件换成了另一份：这里走「写临时文件再 rename 覆盖」，
        // 与部署同一个动作。就地改写（ofstream trunc）不算替换——那是同一个文件对象，创建时间不变
        ASSERT_TRUE(temporaryDirectory.writeFile("incoming.bin", std::string(64U, 'x')));
        std::error_code replaceError;
        std::filesystem::rename(temporaryDirectory.path() / "incoming.bin", targetPath, replaceError);
        ASSERT_FALSE(replaceError) << replaceError.message();

        std::string                                       contents;
        FileBasicInfo                                     opened;
        const std::expected<std::size_t, std::error_code> result = readFileContentsInto(targetPath, 0U, 3U, contents, &opened);

        // 读到 3 字节、短读判据不会响——能认出「发出去的不是那一版」的只有这份身份
        ASSERT_TRUE(result.has_value()) << result.error().message();
        ASSERT_EQ(*result, 3U);
        EXPECT_EQ(contents.size(), 3U);
        EXPECT_NE(opened.sizeBytes, advertised->sizeBytes) << "文件已被换掉，身份却没看出差别，验证器就会被发去描述另一版内容";
#if !ASYN_PLATFORM_WIN32
        // POSIX 的身份标记折了 (设备号, inode, ctime)，认得出这次替换；Windows 用的是创建时间，
        // NTFS 的隧道缓存会把旧文件的创建时间还原到同名新文件上（FileBasicInfo.h 里记着这条限制），
        // 所以那里只有大小与修改秒可用——同大小同秒的替换本平台本来就判不出，不是这次改动带来的退化
        EXPECT_NE(opened.identityTag, advertised->identityTag) << "同一份 inode/ctime 折叠算出了两个值，两条路不同刻度";
#endif
    }

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

        std::string                                       buffer;
        const std::expected<std::size_t, std::error_code> bytesRead = readFileContentsInto(temporaryDirectory.path() / "asset.bin", 2U, 5U, buffer);
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

        std::string                                       buffer("预先占好的一大段内容xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", 40);
        const std::expected<std::size_t, std::error_code> bytesRead = readFileContentsInto(temporaryDirectory.path() / "asset.bin", 0U, 128U, buffer);
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

        std::string                                       buffer;
        const std::expected<std::size_t, std::error_code> bytesRead = readFileContentsInto(missingPath, 0U, 8U, buffer);
        ASSERT_FALSE(bytesRead.has_value()) << "打不开文件不能当成「读到了 0 字节」";
        EXPECT_EQ(bytesRead.error().category(), std::system_category());
    }
} // namespace AsynGyanis::Platform
