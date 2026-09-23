// MemoryMappedFile 单元测试：映射内容、空文件、失败路径与移动语义
#include "Platform/IO/MemoryMappedFile.h"

#include "Platform/FileSystem/FileBasicInfo.h"
#include <optional>

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "PlatformTestSupport.h"

namespace AsynGyanis::Platform
{
    namespace
    {
        /**
         * @brief 把映射视图拷成字符串，便于用 EXPECT_EQ 对账
         * @param mappedFile 已映射的对象
         * @return std::string 映射内容的副本；无效对象返回空串
         */
        std::string mappedText(const MemoryMappedFile &mappedFile)
        {
            const std::span<const std::byte> mappedBytes = mappedFile.bytes();
            return {reinterpret_cast<const char *>(mappedBytes.data()), mappedBytes.size()};
        }
    } // namespace

    /**
     * @brief 映射不存在的文件：返回无效对象并带上系统错误码，视图为空
     */
    TEST(MemoryMappedFile, OpenMissingFileYieldsInvalidObjectWithError)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Mmap_Missing");
        const std::filesystem::path           missingPath = temporaryDirectory.path() / "no-such-file.bin";

        const MemoryMappedFile mappedFile = MemoryMappedFile::open(missingPath);

        EXPECT_FALSE(mappedFile.isValid());
        EXPECT_TRUE(static_cast<bool>(mappedFile.lastError()));
        EXPECT_TRUE(mappedFile.bytes().empty());
    }

    /**
     * @brief 映射已有文件：视图与文件内容逐字节一致，长度按字节数而非字符数
     */
    TEST(MemoryMappedFile, MapsExistingFileContentByteForByte)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Mmap_Content");
        // UTF-8 中文字符每个 3 字节：映射长度若误按字符数计算，这条断言会立刻失败
        const std::string content = "hello 映射 world";
        ASSERT_TRUE(temporaryDirectory.writeFile("payload.txt", content));

        const MemoryMappedFile mappedFile = MemoryMappedFile::open(temporaryDirectory.path() / "payload.txt");

        ASSERT_TRUE(mappedFile.isValid());
        EXPECT_EQ(mappedFile.bytes().size(), content.size());
        EXPECT_EQ(mappedText(mappedFile), content);
    }

    /**
     * @brief 空文件：得到「有效但零字节」的对象，调用方不必特判空文件与失败
     */
    TEST(MemoryMappedFile, EmptyFileIsValidWithEmptyView)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Mmap_Empty");
        ASSERT_TRUE(temporaryDirectory.writeFile("empty.txt", ""));

        const MemoryMappedFile mappedFile = MemoryMappedFile::open(temporaryDirectory.path() / "empty.txt");

        EXPECT_TRUE(mappedFile.isValid());
        EXPECT_FALSE(static_cast<bool>(mappedFile.lastError()));
        EXPECT_TRUE(mappedFile.bytes().empty());
    }

    /**
     * @brief 移动构造：映射转移给新对象，源对象变为无效
     */
    TEST(MemoryMappedFile, MoveConstructionTransfersMapping)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Mmap_MoveCtor");
        const std::string content = "move-существующая-construction";
        ASSERT_TRUE(temporaryDirectory.writeFile("move.txt", content));

        MemoryMappedFile source = MemoryMappedFile::open(temporaryDirectory.path() / "move.txt");
        ASSERT_TRUE(source.isValid());

        const MemoryMappedFile destination(std::move(source));

        EXPECT_TRUE(destination.isValid());
        EXPECT_EQ(mappedText(destination), content);

        // 源对象被搬空后必须失效：若还能读到旧映射，说明句柄/地址没有被排他地转移
        EXPECT_FALSE(source.isValid());
    }

    /**
     * @brief 移动赋值：先释放自己的旧映射，再接管新映射（旧映射不得被漏掉或二次解除）
     */
    TEST(MemoryMappedFile, MoveAssignmentReleasesPreviousMapping)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Mmap_MoveAssign");
        ASSERT_TRUE(temporaryDirectory.writeFile("first.txt", "first-content"));
        ASSERT_TRUE(temporaryDirectory.writeFile("second.txt", "second-content-longer"));

        MemoryMappedFile firstFile  = MemoryMappedFile::open(temporaryDirectory.path() / "first.txt");
        MemoryMappedFile secondFile = MemoryMappedFile::open(temporaryDirectory.path() / "second.txt");
        ASSERT_TRUE(firstFile.isValid());
        ASSERT_TRUE(secondFile.isValid());

        firstFile = std::move(secondFile);

        // 赋值后的对象服务的是新文件；源对象失效
        EXPECT_TRUE(firstFile.isValid());
        EXPECT_EQ(mappedText(firstFile), "second-content-longer");
        EXPECT_FALSE(secondFile.isValid());
    }

    /**
     * @brief 跨页映射：整整 1 MiB 都能读，首字节、页边界与末字节的取值正确
     *
     * @details 视图分页由系统按需建立，映射长度短算、末页缺失这类错误只有在触碰
     *          首字节之外的页时才会暴露，因此专门读几个跨页位置与最后一个字节。
     */
    TEST(MemoryMappedFile, ViewCoversFileBeyondTheFirstPage)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Mmap_Pages");
        constexpr std::size_t kFileLength = 1024 * 1024;

        std::string content;
        content.reserve(kFileLength);
        for (std::size_t index = 0; index < kFileLength; ++index)
        {
            // 只用字母，避免文本写入模式在 Windows 上把 \n 翻译成 CRLF 而改坏字节
            content.push_back(static_cast<char>('A' + (index % 26)));
        }
        ASSERT_TRUE(temporaryDirectory.writeFile("large.txt", content));

        const MemoryMappedFile mappedFile = MemoryMappedFile::open(temporaryDirectory.path() / "large.txt");

        ASSERT_TRUE(mappedFile.isValid());
        const std::span<const std::byte> mappedBytes = mappedFile.bytes();
        ASSERT_EQ(mappedBytes.size(), kFileLength);

        const auto byteAt = [&mappedBytes](const std::size_t index)
        {
            return static_cast<char>(mappedBytes[index]);
        };
        EXPECT_EQ(byteAt(0), content[0]);
        EXPECT_EQ(byteAt(4095), content[4095]);
        EXPECT_EQ(byteAt(4096), content[4096]);
        EXPECT_EQ(byteAt(8192), content[8192]);
        EXPECT_EQ(byteAt(kFileLength - 1), content[kFileLength - 1]);
    }

    /**
     * @brief 映射目录：返回无效对象（Windows 在打开阶段拒绝，POSIX 在映射阶段拒绝）
     */
    /**
     * @brief 钉住：映射交回的身份信息说的就是它映射到的那个对象，且与按路径查同刻度
     * @details 静态服务用它判「已经写下的 ETag 还在不在描述这段正文」。两条路的取值一旦
     *          分叉（换算或身份标记算法不同），每一版正文都会被误判成「不是同一版」而回 500。
     */
    TEST(MemoryMappedFile, OpenedFileInfoMatchesThePathQueryForTheSameObject)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Mmap_OpenedInfo");
        ASSERT_TRUE(temporaryDirectory.writeFile("asset.bin", std::string(4096U, 'q')));
        const std::filesystem::path targetPath = temporaryDirectory.path() / "asset.bin";

        const std::optional<FileBasicInfo> byPath = queryFileBasicInfo(targetPath);
        ASSERT_TRUE(byPath.has_value());

        const MemoryMappedFile mappedFile = MemoryMappedFile::open(targetPath);
        ASSERT_TRUE(mappedFile.isValid()) << mappedFile.lastError().message();
        const std::optional<FileBasicInfo> mappedAs = mappedFile.openedFileInfo();
        ASSERT_TRUE(mappedAs.has_value());
        EXPECT_TRUE(mappedAs->isRegularFile);
        EXPECT_EQ(mappedAs->sizeBytes, byPath->sizeBytes);
        EXPECT_EQ(mappedAs->lastWriteSeconds, byPath->lastWriteSeconds);
        EXPECT_EQ(mappedAs->identityTag, byPath->identityTag) << "两条路的身份标记算法分叉";
    }

    /// 没映射成功的对象无从问身份：给空值而不是给一份凭路径现查的答案（那会把「问的是谁」说反）
    TEST(MemoryMappedFile, OpenedFileInfoIsEmptyForInvalidObject)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Mmap_OpenedInfoMissing");
        const MemoryMappedFile mappedFile =
                MemoryMappedFile::open(temporaryDirectory.path() / "missing.bin");
        ASSERT_FALSE(mappedFile.isValid());
        EXPECT_FALSE(mappedFile.openedFileInfo().has_value());
    }

    TEST(MemoryMappedFile, OpenDirectoryYieldsInvalidObject)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("Mmap_Directory");

        const MemoryMappedFile mappedFile = MemoryMappedFile::open(temporaryDirectory.path());

        EXPECT_FALSE(mappedFile.isValid());
        EXPECT_TRUE(static_cast<bool>(mappedFile.lastError()));
    }
} // namespace AsynGyanis::Platform
