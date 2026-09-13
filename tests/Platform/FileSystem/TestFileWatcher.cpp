/**
 * @file TestFileWatcher.cpp
 * @brief FileWatcher 单元测试：工厂创建、生命周期、事件回调与防抖
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Platform/FileSystem/FileWatcher.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "PlatformTestSupport.h"

namespace AsynGyanis::Platform
{
    namespace
    {
        /**
         * @brief 线程安全的事件记录器，收集监听回调上报的路径与类型
         */
        class FileWatchRecorder
        {
        public:
            /**
             * @brief 作为回调主体记录一次事件
             * @param filePath 变更文件路径
             * @param changeType 变更类型
             */
            void record(const std::string_view filePath, const FileChangeType changeType)
            {
                std::lock_guard lock(m_mutex);
                m_events.emplace_back(std::string(filePath), changeType);
            }

            /**
             * @brief 当前已记录的事件数量
             * @return std::size_t 事件条数
             */
            [[nodiscard]] std::size_t eventCount() const
            {
                std::lock_guard lock(m_mutex);
                return m_events.size();
            }

            /**
             * @brief 是否记录过指定文件名对应的事件
             * @param fileName 文件名（不含目录）
             * @return true 至少有一条事件的路径以该文件名结尾
             */
            [[nodiscard]] bool sawFileNamed(const std::string &fileName) const
            {
                std::lock_guard lock(m_mutex);
                for (const auto &[filePath, changeType]: m_events)
                {
                    (void) changeType;
                    if (filePath.size() >= fileName.size() &&
                        filePath.compare(filePath.size() - fileName.size(), fileName.size(), fileName) == 0)
                    {
                        return true;
                    }
                }
                return false;
            }

        private:
            mutable std::mutex                                   m_mutex;  ///< 保护事件列表
            std::vector<std::pair<std::string, FileChangeType> > m_events; ///< 已记录事件
        };
    } // namespace

    TEST(FileWatcher, CreateReturnsInstanceForCurrentPlatform)
    {
        const std::unique_ptr<FileWatcher> watcher = FileWatcher::create();

        ASSERT_NE(watcher, nullptr);
        EXPECT_FALSE(watcher->isRunning());
    }

    TEST(FileWatcher, StartAndStopToggleRunningState)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileWatcher_Lifecycle");
        const std::unique_ptr<FileWatcher>    watcher = FileWatcher::create();
        ASSERT_NE(watcher, nullptr);

        ASSERT_TRUE(watcher->addWatch(temporaryDirectory.path().string()));
        EXPECT_TRUE(watcher->start());
        EXPECT_TRUE(watcher->isRunning());

        // 重复启动应保持运行状态并返回成功
        EXPECT_TRUE(watcher->start());
        EXPECT_TRUE(watcher->isRunning());

        watcher->stop();
        EXPECT_FALSE(watcher->isRunning());

        // 未运行时停止应安全返回
        EXPECT_NO_THROW(watcher->stop());
    }

    TEST(FileWatcher, ModifyingWatchedFileTriggersCallback)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileWatcher_Modify");
        ASSERT_TRUE(temporaryDirectory.writeFile("watched.yaml", "value: 1\n"));

        const std::unique_ptr<FileWatcher> watcher = FileWatcher::create();
        ASSERT_NE(watcher, nullptr);

        FileWatchRecorder recorder;
        watcher->setDebounceInterval(std::chrono::milliseconds(0));
        watcher->setCallback([&recorder](const std::string_view filePath, const FileChangeType changeType)
        {
            recorder.record(filePath, changeType);
        });

        ASSERT_TRUE(watcher->addWatch(temporaryDirectory.path().string()));
        ASSERT_TRUE(watcher->start());

        // 等待监听就绪后再改写文件
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ASSERT_TRUE(temporaryDirectory.writeFile("watched.yaml", "value: 2\n"));

        const bool receivedEvent = TestSupport::waitForCondition(
                [&recorder]()
                {
                    return recorder.sawFileNamed("watched.yaml");
                },
                3000);

        watcher->stop();
        EXPECT_TRUE(receivedEvent) << "未收到 watched.yaml 的变更回调";
    }

    TEST(FileWatcher, CreatingNewFileInWatchedDirectoryTriggersCallback)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileWatcher_Create");

        const std::unique_ptr<FileWatcher> watcher = FileWatcher::create();
        ASSERT_NE(watcher, nullptr);

        FileWatchRecorder recorder;
        watcher->setDebounceInterval(std::chrono::milliseconds(0));
        watcher->setCallback([&recorder](const std::string_view filePath, const FileChangeType changeType)
        {
            recorder.record(filePath, changeType);
        });

        ASSERT_TRUE(watcher->addWatch(temporaryDirectory.path().string()));
        ASSERT_TRUE(watcher->start());

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ASSERT_TRUE(temporaryDirectory.writeFile("brand-new.yaml", "fresh: true\n"));

        const bool receivedEvent = TestSupport::waitForCondition(
                [&recorder]()
                {
                    return recorder.sawFileNamed("brand-new.yaml");
                },
                3000);

        watcher->stop();
        EXPECT_TRUE(receivedEvent) << "未收到新建文件的变更回调";
    }

    TEST(FileWatcher, RemovedWatchStopsDeliveringCallbacks)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileWatcher_Remove");
        const std::unique_ptr<FileWatcher>    watcher = FileWatcher::create();
        ASSERT_NE(watcher, nullptr);

        FileWatchRecorder recorder;
        watcher->setDebounceInterval(std::chrono::milliseconds(0));
        watcher->setCallback([&recorder](const std::string_view filePath, const FileChangeType changeType)
        {
            recorder.record(filePath, changeType);
        });

        ASSERT_TRUE(watcher->addWatch(temporaryDirectory.path().string()));
        EXPECT_TRUE(watcher->removeWatch(temporaryDirectory.path().string()));
        EXPECT_FALSE(watcher->removeWatch(temporaryDirectory.path().string()));

        ASSERT_TRUE(watcher->start());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ASSERT_TRUE(temporaryDirectory.writeFile("ignored.yaml", "ignored: true\n"));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        watcher->stop();

        EXPECT_EQ(recorder.eventCount(), 0U);
    }

    TEST(FileWatcher, AddWatchOnMissingDirectoryFails)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileWatcher_Missing");
        const std::filesystem::path           missingDirectory = temporaryDirectory.path() / "not_created";

        const std::unique_ptr<FileWatcher> watcher = FileWatcher::create();
        ASSERT_NE(watcher, nullptr);

        EXPECT_FALSE(watcher->addWatch(missingDirectory.string()));
    }

    TEST(FileWatcher, AddingSameDirectoryTwiceIsIdempotent)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileWatcher_Duplicate");
        const std::unique_ptr<FileWatcher>    watcher = FileWatcher::create();
        ASSERT_NE(watcher, nullptr);

        EXPECT_TRUE(watcher->addWatch(temporaryDirectory.path().string()));
        EXPECT_TRUE(watcher->addWatch(temporaryDirectory.path().string()));
    }

    TEST(FileWatcher, DebounceIntervalSuppressesRapidRepeatedChanges)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileWatcher_Debounce");
        ASSERT_TRUE(temporaryDirectory.writeFile("debounced.yaml", "round: 1\n"));

        const std::unique_ptr<FileWatcher> watcher = FileWatcher::create();
        ASSERT_NE(watcher, nullptr);

        FileWatchRecorder recorder;
        watcher->setDebounceInterval(std::chrono::seconds(30));
        watcher->setCallback([&recorder](const std::string_view filePath, const FileChangeType changeType)
        {
            recorder.record(filePath, changeType);
        });

        ASSERT_TRUE(watcher->addWatch(temporaryDirectory.path().string()));
        ASSERT_TRUE(watcher->start());

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ASSERT_TRUE(temporaryDirectory.writeFile("debounced.yaml", "round: 2\n"));
        ASSERT_TRUE(TestSupport::waitForCondition(
            [&recorder]()
            {
            return recorder.eventCount() >= 1;
            },
            3000));

        // 防抖窗口内继续改写，不应产生第二条事件
        ASSERT_TRUE(temporaryDirectory.writeFile("debounced.yaml", "round: 3\n"));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        watcher->stop();

        EXPECT_EQ(recorder.eventCount(), 1U);
    }

    TEST(FileWatcher, RecursiveWatchRegistersSubDirectories)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("FileWatcher_Recursive");
        std::error_code                 error;
        std::filesystem::create_directories(temporaryDirectory.path() / "sub", error);
        ASSERT_FALSE(error);

        std::unique_ptr<FileWatcher> watcher = FileWatcher::create();
        ASSERT_NE(watcher, nullptr);

        FileWatchRecorder recorder;
        watcher->setDebounceInterval(std::chrono::milliseconds(0));
        watcher->setCallback([&recorder](const std::string_view filePath, const FileChangeType changeType)
        {
            recorder.record(filePath, changeType);
        });

        ASSERT_TRUE(watcher->addWatch(temporaryDirectory.path().string(), true));
        ASSERT_TRUE(watcher->start());

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // 子目录内的文件写入应被下层监听捕获
        {
            std::ofstream subFile(temporaryDirectory.path() / "sub" / "nested.yaml");
            subFile << "nested: true\n";
        }

        const bool receivedEvent = TestSupport::waitForCondition(
                [&recorder]()
                {
                    return recorder.sawFileNamed("nested.yaml");
                },
                3000);

        watcher->stop();
        EXPECT_TRUE(receivedEvent) << "递归监听未能覆盖子目录中的文件";
    }

    TEST(FileWatcher, DestructorStopsRunningWatcherSafely)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("FileWatcher_Destructor");
        {
            // recorder 必须先于 watcher 声明：作用域退出时按声明逆序销毁，watcher 先析构
            // （停线程并等它退出），回调不可能再触碰 recorder；反过来写则是 recorder 先
            // 销毁，watcher 线程的最后一次回调会写进已析构的对象（LeakSanitizer 报泄漏）
            FileWatchRecorder recorder;

            const std::unique_ptr<FileWatcher> watcher = FileWatcher::create();
            ASSERT_NE(watcher, nullptr);
            watcher->setCallback([&recorder](const std::string_view filePath, const FileChangeType changeType)
            {
                recorder.record(filePath, changeType);
            });

            ASSERT_TRUE(watcher->addWatch(temporaryDirectory.path().string()));
            ASSERT_TRUE(watcher->start());
            ASSERT_TRUE(temporaryDirectory.writeFile("before-destroy.yaml", "x: 1\n"));
            // 故意不调用 stop()，析构必须自行停线程且不崩溃
        }
        SUCCEED();
    }
} // namespace AsynGyanis::Platform
