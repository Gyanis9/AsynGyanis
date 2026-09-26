#include "Net/Http/StaticFileMappingCache.h"

#include "NetTestSupport.h"

#include "Platform/FileSystem/FileBasicInfo.h"
#include "Platform/IO/MemoryMappedFile.h"

#include <gtest/gtest.h>

#include <barrier>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using TestSupport::TemporaryFile;

        /**
         * @brief 造一份命中判据用的元数据：缓存只认「大小 + 修改秒 + 身份标记」这三个数
         */
        [[nodiscard]] Platform::FileBasicInfo makeStamp(const std::uintmax_t sizeBytes, const std::int64_t lastWriteSeconds, const std::uint64_t identityTag = 7)
        {
            Platform::FileBasicInfo info;
            info.isRegularFile    = true;
            info.sizeBytes        = sizeBytes;
            info.lastWriteSeconds = lastWriteSeconds;
            info.identityTag      = identityTag;
            return info;
        }

        /**
         * @brief 建一份真实映射并包成缓存所要求的共享所有权形态
         */
        [[nodiscard]] std::shared_ptr<const Platform::MemoryMappedFile> openMapping(const std::filesystem::path &filePath)
        {
            auto mapping = std::make_shared<Platform::MemoryMappedFile>(Platform::MemoryMappedFile::open(filePath));
            EXPECT_TRUE(mapping->isValid());
            return mapping;
        }
    } // namespace

    /**
     * @brief 未登记过的路径不命中：缓存不能凭空造出一份映射
     */
    TEST(StaticFileMappingCache, ReturnsNullForUnknownPath)
    {
        StaticFileMappingCache cache{4};

        EXPECT_EQ(cache.find(std::filesystem::path("never-stored.bin"), makeStamp(10, 100)), nullptr);
        EXPECT_EQ(cache.entryCount(), 0U);
    }

    /**
     * @brief 元数据一致时命中，并交出同一份映射（不是重新打开的第二个视图）
     */
    TEST(StaticFileMappingCache, ReusesTheSameMappingWhileMetadataMatches)
    {
        const TemporaryFile                                     temporaryFile("CacheHit", "0123456789");
        const std::shared_ptr<const Platform::MemoryMappedFile> mapping = openMapping(temporaryFile.path());

        StaticFileMappingCache cache{4};
        cache.store(temporaryFile.path(), mapping, makeStamp(10, 100));

        const std::shared_ptr<const Platform::MemoryMappedFile> found = cache.find(temporaryFile.path(), makeStamp(10, 100));
        ASSERT_NE(found, nullptr);
        EXPECT_EQ(found.get(), mapping.get());
    }

    /**
     * @brief 大小变了即不命中，且旧条目被就地摘掉，不给下一次留下可命中的残留
     */
    TEST(StaticFileMappingCache, DropsEntryWhenSizeChanges)
    {
        const TemporaryFile                                     temporaryFile("CacheSize", "0123456789");
        const std::shared_ptr<const Platform::MemoryMappedFile> mapping = openMapping(temporaryFile.path());

        StaticFileMappingCache cache{4};
        cache.store(temporaryFile.path(), mapping, makeStamp(10, 100));

        EXPECT_EQ(cache.find(temporaryFile.path(), makeStamp(11, 100)), nullptr);
        EXPECT_EQ(cache.entryCount(), 0U);
    }

    /**
     * @brief 长度与修改秒都一样、但文件身份不同（原子替换）时必须不命中：只比 size+mtime 会发旧字节
     */
    TEST(StaticFileMappingCache, DropsEntryWhenFileIdentityChanges)
    {
        const TemporaryFile                                     temporaryFile("CacheIdentity", "0123456789");
        const std::shared_ptr<const Platform::MemoryMappedFile> mapping = openMapping(temporaryFile.path());

        StaticFileMappingCache cache{4};
        cache.store(temporaryFile.path(), mapping, makeStamp(10, 100, 7));

        EXPECT_EQ(cache.find(temporaryFile.path(), makeStamp(10, 100, 8)), nullptr);
        EXPECT_EQ(cache.entryCount(), 0U);
    }

    /**
     * @brief 修改时间变了同样不命中，且旧条目被就地摘掉，不给下一次留下可命中的残留
     */
    TEST(StaticFileMappingCache, DropsEntryWhenModificationTimeChanges)
    {
        const TemporaryFile                                     temporaryFile("CacheMtime", "0123456789");
        const std::shared_ptr<const Platform::MemoryMappedFile> mapping = openMapping(temporaryFile.path());

        StaticFileMappingCache cache{4};
        cache.store(temporaryFile.path(), mapping, makeStamp(10, 100));
        ASSERT_NE(cache.find(temporaryFile.path(), makeStamp(10, 100)), nullptr);

        EXPECT_EQ(cache.find(temporaryFile.path(), makeStamp(10, 101)), nullptr);
        EXPECT_EQ(cache.entryCount(), 0U);

        // 重新登记新元数据之后应当恢复命中：淘汰不是禁用
        cache.store(temporaryFile.path(), mapping, makeStamp(10, 101));
        EXPECT_NE(cache.find(temporaryFile.path(), makeStamp(10, 101)), nullptr);
    }

    /**
     * @brief 超出上限时按「最久未用」淘汰，而不是按登记先后：命中要提升位置
     */
    TEST(StaticFileMappingCache, EvictsLeastRecentlyUsedBeyondCapacity)
    {
        const TemporaryFile firstFile("CacheLruFirst", "a");
        const TemporaryFile secondFile("CacheLruSecond", "b");
        const TemporaryFile thirdFile("CacheLruThird", "c");

        StaticFileMappingCache cache{2};
        cache.store(firstFile.path(), openMapping(firstFile.path()), makeStamp(1, 1));
        cache.store(secondFile.path(), openMapping(secondFile.path()), makeStamp(1, 1));
        // 先访问第一条，让它升到最近使用；此后塞第三条该淘汰的是第二条
        ASSERT_NE(cache.find(firstFile.path(), makeStamp(1, 1)), nullptr);
        cache.store(thirdFile.path(), openMapping(thirdFile.path()), makeStamp(1, 1));

        EXPECT_EQ(cache.entryCount(), 2U);
        EXPECT_NE(cache.find(firstFile.path(), makeStamp(1, 1)), nullptr);
        EXPECT_EQ(cache.find(secondFile.path(), makeStamp(1, 1)), nullptr);
        EXPECT_NE(cache.find(thirdFile.path(), makeStamp(1, 1)), nullptr);
    }

    /**
     * @brief 上限为 0 即整条关闭：既不写入也永不命中，静态路由退回每请求现建映射
     */
    TEST(StaticFileMappingCache, TreatsZeroCapacityAsDisabled)
    {
        const TemporaryFile temporaryFile("CacheDisabled", "abc");

        StaticFileMappingCache cache{0};
        cache.store(temporaryFile.path(), openMapping(temporaryFile.path()), makeStamp(3, 7));

        EXPECT_EQ(cache.entryCount(), 0U);
        EXPECT_EQ(cache.find(temporaryFile.path(), makeStamp(3, 7)), nullptr);
        EXPECT_EQ(cache.maximumEntryCount(), 0U);
    }

    /**
     * @brief 空映射不占名额：映射失败不该把缓存挤满
     */
    TEST(StaticFileMappingCache, IgnoresNullMappingWhenStoring)
    {
        StaticFileMappingCache cache{2};

        cache.store(std::filesystem::path("missing.bin"), nullptr, makeStamp(0, 0));

        EXPECT_EQ(cache.entryCount(), 0U);
    }

    /**
     * @brief 同一路径重复登记是替换而不是并存，名额不会被同一文件吃掉两份
     */
    TEST(StaticFileMappingCache, ReplacesExistingPathWithoutGrowingEntryCount)
    {
        const TemporaryFile firstFile("CacheReplaceA", "aaaa");
        const TemporaryFile secondFile("CacheReplaceB", "bb");

        StaticFileMappingCache cache{2};
        cache.store(firstFile.path(), openMapping(firstFile.path()), makeStamp(4, 1));
        cache.store(firstFile.path(), openMapping(secondFile.path()), makeStamp(2, 2));

        EXPECT_EQ(cache.entryCount(), 1U);
        const std::shared_ptr<const Platform::MemoryMappedFile> found = cache.find(firstFile.path(), makeStamp(2, 2));
        ASSERT_NE(found, nullptr);
        EXPECT_EQ(found->bytes().size(), 2U);
    }

    /**
     * @brief 多个循环线程并发读写的不变式：不崩、条目数不越上限
     * @details 用两道栅栏真正造出重叠（并发用例不得赌调度），上限压到 3 而路径有 6 条，
     *          保证淘汰与插入在轮次里真的交错
     */
    TEST(StaticFileMappingCache, StaysWithinCapacityUnderConcurrentFindAndStore)
    {
        constexpr std::size_t threadCount = 4;
        constexpr std::size_t iterations  = 200;
        constexpr std::size_t pathCount   = 6;

        // 夹具带用户声明的析构函数，因此不可移动：并发用例要按数量造，就用所有权指针装着
        std::vector<std::unique_ptr<TemporaryFile>> files;
        files.reserve(pathCount);
        for (std::size_t index = 0; index < pathCount; ++index)
        {
            files.push_back(std::make_unique<TemporaryFile>("CacheConcurrent" + std::to_string(index), std::string(4, static_cast<char>('a' + index))));
        }

        std::vector<std::shared_ptr<const Platform::MemoryMappedFile>> mappings;
        mappings.reserve(pathCount);
        for (const std::unique_ptr<TemporaryFile> &file: files)
        {
            mappings.push_back(openMapping(file->path()));
        }

        StaticFileMappingCache   cache{3};
        std::barrier             reuseBarrier{threadCount};
        std::barrier             finishBarrier{threadCount};
        std::vector<std::thread> workers;
        workers.reserve(threadCount);
        for (std::size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex)
        {
            workers.emplace_back(
                    [&cache, &files, &mappings, &reuseBarrier, &finishBarrier, threadIndex]
                    {
                        reuseBarrier.arrive_and_wait();
                        for (std::size_t iteration = 0; iteration < iterations; ++iteration)
                        {
                            const std::size_t slot = (threadIndex * 3 + iteration) % pathCount;
                            // 读写交替，且元数据每 5 轮故意错一次，把「不命中即摘条目」的分支也压进并发里
                            const std::uintmax_t sizeBytes = iteration % 5 == 4 ? 999 : 4;
                            if (cache.find(files[slot]->path(), makeStamp(sizeBytes, 1)) == nullptr)
                            {
                                cache.store(files[slot]->path(), mappings[slot], makeStamp(sizeBytes, 1));
                            }
                        }
                        finishBarrier.arrive_and_wait();
                    });
        }
        for (std::thread &worker: workers)
        {
            worker.join();
        }

        EXPECT_LE(cache.entryCount(), 3U);
    }

    /**
     * @brief 登记的命中判据按映射的真实长度成形，而不是照抄调用方那份查询读数
     * @details 查询与建映射之间文件被改大时两者会不一致。若照抄调用方的读数，就留下一条
     *          「键说 4 字节、正文却能给 10 字节」的条目；日后一次在同一秒内把文件截断回 4 字节的
     *          查询会命中它（三项判据全对上），按映射长度发正文就踩到已随截断解除的页。
     *          本用例两个方向各钉一次：过期读数不得命中，映射长度那份读数必须命中。
     */
    TEST(StaticFileMappingCache, KeysEntryByMappingLengthRatherThanByCallerStamp)
    {
        const TemporaryFile                                     temporaryFile("StaleStamp", "0123456789");
        const std::shared_ptr<const Platform::MemoryMappedFile> mapping = openMapping(temporaryFile.path());
        ASSERT_EQ(mapping->bytes().size(), 10U);

        {
            StaticFileMappingCache cache{4};
            cache.store(temporaryFile.path(), mapping, makeStamp(4, 100));
            EXPECT_NE(cache.find(temporaryFile.path(), makeStamp(10, 100)), nullptr) << "键必须描述这份映射真能交出的字节数，否则映射长度那份读数永远命不中";
        }

        {
            StaticFileMappingCache cache{4};
            cache.store(temporaryFile.path(), mapping, makeStamp(4, 100));
            // 判据不一致时 find 会就地摘掉条目，因此这里既要不命中、也要看到表被清空
            EXPECT_EQ(cache.find(temporaryFile.path(), makeStamp(4, 100)), nullptr) << "调用方的过期读数被照抄进键，就会允许「键 4 字节 / 正文 10 字节」的条目存在";
            EXPECT_EQ(cache.entryCount(), 0U);
        }
    }
} // namespace AsynGyanis::Net
