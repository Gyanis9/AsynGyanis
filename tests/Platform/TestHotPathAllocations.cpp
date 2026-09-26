// Platform 侧「每个请求都要付一次」的原语各付出多少次堆分配。计时归 benchmarks/microbench，这里只数分配。
//
// 量法与三条纪律同 tests/Net/Http/TestHotPathAllocations.cpp（共用 AllocationProbe）：被测体里不放断言
// （gtest 造判词本身要分配），改成返回一个与「做成了多少」成正比的标记；先量一次什么都不做的本底；
// 计数件自检。凡声称「稳态零分配」的形状，判据一律取一千次的原值——摊平是整除，「每次 0 次」这个读数
// 掩盖得住一千次里的 999 次分配。
//
// 读数（Release / MSVC；括号内是 Debug 下同一形状的读数，只作对照）：
//   · queryFileBasicInfo（存在的文件、不存在的路径各一条）：一千次共 0 次。一次系统调用同时给出类型、
//     大小、修改秒与身份标记，产物是不带堆成员的 optional 值（Debug 同为 0）；
//   · MemoryMappedFile::open 到析构：0 次（Debug 0）。对象只装句柄、长度与 error_code，正文按视图交出，
//     聚合体本身不在堆上；调用本身要付的系统调用不在本台账口径里；
//   · MemoryMappedFile::openedFileInfo 与 readFileContentsInto(带身份出参)：各 0 次。静态服务为核对
//     「正文与验证器同版本」每请求多问的那一次身份，产物同样是一份不带堆成员的 optional 值；
//   · UTF-8→path 与 path→UTF-8 各 1 次 / 64 与 32 字节（ASCII 名；非 ASCII 名 1 次 / 48 与 32 字节）。
//     那一次就是产物本身的缓冲——按值交出一段新文本没有更省的形状了。Debug 下同一形状是 4 次与 3 次，
//     差的是 STL 调试期的中间量，不是实现退化。扩展名只有几字符时产物进小串内联，因此「只把 extension()
//     交给 MIME 查询」这类改法能把每请求那一次也省掉（已这么落：静态服务的 MIME 查询实测从整条路径的
//     1 次 / 112 字节降到只取出串名的 0 次，Release MSVC；容器 GCC 同形状 1 次 / 0 次）。

#include "Platform/FileSystem/FileBasicInfo.h"
#include "Platform/FileSystem/FileSystem.h"
#include "Platform/IO/FileContents.h"
#include "Platform/IO/MemoryMappedFile.h"

#include "AllocationProbe.h"
#include "PlatformTestSupport.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace AsynGyanis::Platform
{
    namespace
    {
        // 轮数常量、AllocationProfile 与 measurePerOperation 都来自共用探针（AllocationProbe.h）
        using AsynGyanis::TestSupport::AllocationProfile;
        using AsynGyanis::TestSupport::kMeasurementIterations;
        using AsynGyanis::TestSupport::measurePerOperation;

        /// 台账自用的正文字节数：够小，让一千轮映射建立与解除留在毫秒量级
        constexpr std::size_t kLedgerFileBytes = 4096U;

#ifdef NDEBUG
        // 名字里带 Total 的钉的是「一千次一共多少次」（原值），不是摊平读数
        constexpr std::uint64_t kFileBasicInfoTotalAllocationsPerThousand      = 0U; ///< 静态文件每请求都要查的那一次
        constexpr std::uint64_t kMappedFileOpenTotalAllocationsPerThousand     = 0U; ///< 映射未命中时才付，但同样每请求都可能付
        constexpr std::uint64_t kOpenedFileInfoTotalAllocationsPerThousand     = 0U; ///< 核对「正文与验证器同版本」时每请求要问的那一次
        constexpr std::uint64_t kPathTextConversionTotalAllocationsPerThousand = 0U; ///< 只取扩展名那条：产物短到留在内联缓冲里
#endif
    } // namespace

    /**
     * @brief 计数件要证明自己看得见分配
     * @details 少了这条，哪天替换件被链接顺序顶掉，本文件所有读数都会是 0，而 0 看着像「零分配的好实现」
     */
    TEST(PlatformHotPathAllocations, CountingHookSeesAPlainHeapAllocation)
    {
        const AllocationProfile profile = measurePerOperation(
                []
                {
                    // 64 字节超过短串内联缓冲，每次都要向堆要一次
                    const std::string allocated(64, 'x');
                    return allocated.size();
                });
        EXPECT_GE(profile.allocationsPerOperation, 1U) << "operator new 的替换件没生效，本文件所有读数都不可信";
        EXPECT_GE(profile.bytesPerOperation, 64U);
        EXPECT_EQ(profile.resultSum, kMeasurementIterations * 64U);
    }

    /**
     * @brief 测量窗自身的本底：什么都不做的循环必须量出 0 次分配
     */
    TEST(PlatformHotPathAllocations, MeasurementWindowHasNoBackgroundAllocations)
    {
        std::uint64_t           sink    = 0;
        const AllocationProfile profile = measurePerOperation(
                [&sink]
                {
                    // 留一条对 sink 的写，编译器就不能把整段循环判成空转删掉
                    sink += 1U;
                    return std::size_t{0};
                });
        EXPECT_EQ(profile.totalAllocations, 0U) << "测量窗里有背景分配，形状读数不可信";
        EXPECT_EQ(sink, kMeasurementIterations);
    }

    /**
     * @brief 读一次文件基本信息付出多少次分配（成功与查不到两条路各量一次）
     * @details 静态文件服务每个请求都要这一份元数据来出 ETag、Last-Modified 与大小，是 Platform 侧
     *          最贴近「每请求一次」的原语。失败那条也要量：它走的是 error_code 与 empty optional，
     *          一旦有人在这里现造诊断串，探测不到的路径就会开始碰堆
     */
    TEST(PlatformHotPathAllocations, FileBasicInfoQueryAllocations)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("HotPathAllocations_FileBasicInfo");
        ASSERT_TRUE(temporaryDirectory.writeFile("asset.bin", std::string(kLedgerFileBytes, 'x')));
        const std::filesystem::path existingPath = temporaryDirectory.path() / "asset.bin";
        const std::filesystem::path missingPath  = temporaryDirectory.path() / "does-not-exist.bin";

        ASSERT_TRUE(queryFileBasicInfo(existingPath).has_value()) << "夹具文件没建出来，读数没意义";
        ASSERT_FALSE(queryFileBasicInfo(missingPath).has_value()) << "缺失路径不该查得出东西";

        const auto queryExistingOnce = [&existingPath]
        {
            const std::optional<FileBasicInfo> info = queryFileBasicInfo(existingPath);
            return info.has_value() ? static_cast<std::size_t>(info->sizeBytes) : 0U;
        };
        const AllocationProfile existing = measurePerOperation(queryExistingOnce);
        EXPECT_EQ(existing.resultSum, kMeasurementIterations * kLedgerFileBytes) << "有几次没读到大小，读的不是那条形状";

        const auto queryMissingOnce = [&missingPath]
        {
            static_cast<void>(queryFileBasicInfo(missingPath));
            return std::size_t{0};
        };
        const AllocationProfile missing = measurePerOperation(queryMissingOnce);

        std::printf("queryFileBasicInfo 命中：每次 %llu 次 / %llu 字节（一千次共 %llu 次）；查不到：每次 %llu 次 / 一千次共 %llu 次\n",
                    static_cast<unsigned long long>(existing.allocationsPerOperation), static_cast<unsigned long long>(existing.bytesPerOperation),
                    static_cast<unsigned long long>(existing.totalAllocations), static_cast<unsigned long long>(missing.allocationsPerOperation),
                    static_cast<unsigned long long>(missing.totalAllocations));
#ifdef NDEBUG
        EXPECT_EQ(existing.totalAllocations, kFileBasicInfoTotalAllocationsPerThousand) << "每请求一次的元数据查询开始碰堆：多半是有人在这里现造了路径文本或诊断串";
        EXPECT_EQ(missing.totalAllocations, kFileBasicInfoTotalAllocationsPerThousand) << "查不到那条路也开始碰堆，失败路径的分配比成功路径更难被压测看见";
#endif
    }

    /**
     * @brief 建立并解除一次文件映射付出多少次分配
     * @details 映射缓存未命中时每个请求都要走一遍 open；对象本身按值返回，堆上不该有任何东西
     */
    TEST(PlatformHotPathAllocations, MappedFileOpenAllocations)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("HotPathAllocations_MappedFile");
        ASSERT_TRUE(temporaryDirectory.writeFile("mapped.bin", std::string(kLedgerFileBytes, 'y')));
        const std::filesystem::path targetPath = temporaryDirectory.path() / "mapped.bin";

        const auto openOnce = [&targetPath]
        {
            MemoryMappedFile  mappedFile  = MemoryMappedFile::open(targetPath);
            const std::size_t mappedBytes = mappedFile.isValid() ? mappedFile.bytes().size() : 0U;
            return mappedBytes;
        };
        ASSERT_EQ(openOnce(), kLedgerFileBytes) << "映射没建立成功，读数没意义";

        const AllocationProfile profile = measurePerOperation(openOnce);
        EXPECT_EQ(profile.resultSum, kMeasurementIterations * kLedgerFileBytes) << "有几次映射长度不一致";
        std::printf("MemoryMappedFile::open+close 每次分配 %llu 次 / %llu 字节（一千次共 %llu 次）\n", static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.bytesPerOperation), static_cast<unsigned long long>(profile.totalAllocations));
#ifdef NDEBUG
        EXPECT_EQ(profile.totalAllocations, kMappedFileOpenTotalAllocationsPerThousand) << "映射对象开始带堆成员了：它只该装句柄、长度与 error_code";
#endif
    }

    /**
     * @brief 从已打开的对象问一次身份付出多少次分配（映射与整段读两条形状各量一次）
     * @details 静态文件服务为防止「验证器描述旧版本、正文是新版本」，每请求都要在关掉句柄之前问一次
     *          身份。这两条就是那条新查问的全部产物开销：Windows 侧走整段读，POSIX 侧走映射
     */
    TEST(PlatformHotPathAllocations, OpenedFileInfoQueryAllocations)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("HotPathAllocations_OpenedFileInfo");
        ASSERT_TRUE(temporaryDirectory.writeFile("ledger.bin", std::string(kLedgerFileBytes, 'z')));
        const std::filesystem::path targetPath = temporaryDirectory.path() / "ledger.bin";

        const MemoryMappedFile mappedFile = MemoryMappedFile::open(targetPath);
        ASSERT_TRUE(mappedFile.isValid()) << "映射没建立成功，读数没意义";

        const auto queryOnce = [&mappedFile]
        {
            const std::optional<FileBasicInfo> info = mappedFile.openedFileInfo();
            return info.has_value() ? static_cast<std::size_t>(info->sizeBytes) : 0U;
        };
        ASSERT_EQ(queryOnce(), kLedgerFileBytes);
        const AllocationProfile mappedQuery = measurePerOperation(queryOnce);
        EXPECT_EQ(mappedQuery.resultSum, kMeasurementIterations * kLedgerFileBytes) << "有几次问不出大小，读的不是那条形状";

        // 整段读那条要预先备好缓冲：稳态下 resize 只改长度，这里量的是「读 + 问身份」这一段
        std::string   body(kLedgerFileBytes, '\0');
        FileBasicInfo openedAs;
        const auto    readOnce = [&targetPath, &body, &openedAs]
        {
            const std::expected<std::size_t, std::error_code> read = readFileContentsInto(targetPath, 0U, kLedgerFileBytes, body, &openedAs);
            return read.has_value() ? *read : 0U;
        };
        ASSERT_EQ(readOnce(), kLedgerFileBytes) << "读不出整段正文，读数没意义";
        const AllocationProfile readWithIdentity = measurePerOperation(readOnce);
        EXPECT_EQ(readWithIdentity.resultSum, kMeasurementIterations * kLedgerFileBytes) << "有几次没读满，读的不是那条形状";

        std::printf("openedFileInfo 每次 %llu 次 / %llu 字节；readFileContentsInto(带身份) 每次 %llu 次 / %llu 字节\n",
                    static_cast<unsigned long long>(mappedQuery.allocationsPerOperation), static_cast<unsigned long long>(mappedQuery.bytesPerOperation),
                    static_cast<unsigned long long>(readWithIdentity.allocationsPerOperation), static_cast<unsigned long long>(readWithIdentity.bytesPerOperation));
#ifdef NDEBUG
        EXPECT_EQ(mappedQuery.totalAllocations, kOpenedFileInfoTotalAllocationsPerThousand) << "每请求一次的身份查询开始碰堆：产物只该是一份不带堆成员的 optional 值";
        EXPECT_EQ(readWithIdentity.totalAllocations, kOpenedFileInfoTotalAllocationsPerThousand) << "整段读带身份核对不再是稳态零分配：多半是失败路径开始现造诊断串";
#endif
    }

    /**
     * @brief 只为查 MIME 而把路径出成 UTF-8 文本：整条路径与只取扩展名两种形状各量一次
     * @details 静态文件服务每请求都要查一次 MIME，而查表只看最后一段扩展名。扩展名短到能留在小串
     *          内联缓冲里（MSVC 是 15 字节），因此「只出扩展名」这条稳态零分配；整条路径出串则按路径
     *          长度线性碰堆。对照形状同时量：0 读数有一部分来自内联缓冲，把扩展名拉长到超出内联就要
     *          重新碰堆，只看「短扩展名 0 次」会把这种依赖当成实现保证。
     */
    TEST(PlatformHotPathAllocations, MimeTypePathTextConversionAllocations)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("HotPathAllocations_MimeTypeText");
        ASSERT_TRUE(temporaryDirectory.writeFile("app.min.js", "x"));
        const std::filesystem::path fullPath  = temporaryDirectory.path() / "app.min.js";
        const std::filesystem::path extension = fullPath.extension();
        // 拉长到超出小串内联的扩展名，作为「0 次是靠内联缓冲」的对照（取的是同一条形状：扩展名本身）
        const std::filesystem::path longExtension = std::filesystem::path("archive.thisisareallylongextension").extension();

        const auto wholePathOnce     = [&fullPath] { return FileSystem::utf8FromPath(fullPath).size(); };
        const auto extensionOnce     = [&extension] { return FileSystem::utf8FromPath(extension).size(); };
        const auto longExtensionOnce = [&longExtension] { return FileSystem::utf8FromPath(longExtension).size(); };

        const AllocationProfile wholePath   = measurePerOperation(wholePathOnce);
        const AllocationProfile shortSuffix = measurePerOperation(extensionOnce);
        const AllocationProfile longSuffix  = measurePerOperation(longExtensionOnce);
        // 产物长度自己算：这两条判据要证的是「一千轮都在读同一条形状」，不是某个手写常数
        const std::size_t shortSuffixBytes = FileSystem::utf8FromPath(extension).size();
        const std::size_t longSuffixBytes  = FileSystem::utf8FromPath(longExtension).size();
        EXPECT_EQ(shortSuffix.resultSum, kMeasurementIterations * shortSuffixBytes) << "读的不是那条扩展名形状";
        EXPECT_EQ(longSuffix.resultSum, kMeasurementIterations * longSuffixBytes);
        ASSERT_GT(longSuffixBytes, shortSuffixBytes) << "对照形状没拉长，比不出内联缓冲的那一层";

        std::printf("出 MIME 用的路径文本：整条路径（%zu 字符）每次 %llu 次 / %llu 字节；只取扩展名（%zu 字符）%llu 次；"
                    "超出内联的长扩展名（%zu 字符）%llu 次\n",
                    FileSystem::utf8FromPath(fullPath).size(), static_cast<unsigned long long>(wholePath.allocationsPerOperation),
                    static_cast<unsigned long long>(wholePath.bytesPerOperation), shortSuffixBytes, static_cast<unsigned long long>(shortSuffix.allocationsPerOperation),
                    longSuffixBytes, static_cast<unsigned long long>(longSuffix.allocationsPerOperation));
#ifdef NDEBUG
        EXPECT_EQ(shortSuffix.totalAllocations, kPathTextConversionTotalAllocationsPerThousand) << "只取扩展名的那条不该碰堆：产物长度只有 3，超出内联缓冲之前不该有分配";
        EXPECT_GT(wholePath.totalAllocations, 0U) << "整条路径出串那条一直是按长度碰堆的，读数为 0 说明探针没生效";
#endif
    }

    /**
     * @brief 路径文本与路径对象互换的各方向付出多少次分配（只打印，不钉数）
     * @details 静态目录配置、日志与报错文案都走这一对，Windows 侧还要在每请求的 MIME 查询上过一遍。
     *          四个方向分开量：两平台的刻度不同（Windows 过码表、POSIX 纯拷贝），钉死任何一个数都是
     *          钉实现，这里只把读数留在原地供「换写法前后」对照。
     */
    TEST(PlatformHotPathAllocations, PathTextConversionAllocations)
    {
        const std::string           asciiPath          = "wwwroot/assets/app.min.js";
        const std::string           nonAsciiPath       = std::string("\xE6\x96\x87") + "\xE4\xBB\xB6/assets/\xE6\x8A\xA5\xE5\x91\x8A.txt";
        const std::filesystem::path asciiPathObject    = FileSystem::pathFromUtf8(asciiPath);
        const std::filesystem::path nonAsciiPathObject = FileSystem::pathFromUtf8(nonAsciiPath);

        // 每条形状各跑一千次后核对「产物长度总和」，证明编译器没把那次转换当成空转删掉
        const auto measureConversion = [](const std::string &utf8Text, const std::filesystem::path &pathObject)
        {
            const auto              toPathOnce = [&utf8Text] { return FileSystem::pathFromUtf8(utf8Text).native().size(); };
            const auto              toTextOnce = [&pathObject] { return FileSystem::utf8FromPath(pathObject).size(); };
            const AllocationProfile toPath     = measurePerOperation(toPathOnce);
            const AllocationProfile toText     = measurePerOperation(toTextOnce);
            EXPECT_EQ(toPath.resultSum, kMeasurementIterations * pathObject.native().size());
            EXPECT_EQ(toText.resultSum, kMeasurementIterations * utf8Text.size());
            std::printf("  UTF-8→path %llu 次 / %llu 字节；path→UTF-8 %llu 次 / %llu 字节（各一千次共 %llu / %llu 次）\n",
                        static_cast<unsigned long long>(toPath.allocationsPerOperation), static_cast<unsigned long long>(toPath.bytesPerOperation),
                        static_cast<unsigned long long>(toText.allocationsPerOperation), static_cast<unsigned long long>(toText.bytesPerOperation),
                        static_cast<unsigned long long>(toPath.totalAllocations), static_cast<unsigned long long>(toText.totalAllocations));
        };

        std::printf("pathFromUtf8/utf8FromPath（ASCII 名 %zu 字符）\n", asciiPath.size());
        measureConversion(asciiPath, asciiPathObject);
        std::printf("pathFromUtf8/utf8FromPath（非 ASCII 名 %zu 字符）\n", nonAsciiPath.size());
        measureConversion(nonAsciiPath, nonAsciiPathObject);
    }
} // namespace AsynGyanis::Platform
