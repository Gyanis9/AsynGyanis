// FileWatcher 单元测试：工厂创建、生命周期、事件回调与防抖
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

    /**
     * @brief 钉住：撤销一条**递归**监视之后，它不会被自愈逻辑悄悄加回来
     * @details 递归根另存了一份清单，供「目录被删掉后又回来」时补挂监视用。撤销一条递归监视时
     *          若只清监视表、不清这份清单，下一拍自愈就会把它重新挂上：removeWatch() 明明返回了
     *          true，句柄却重开、回调照旧派发——等于把调用方的显式撤销否决掉。
     *          等待时长刻意跨过至少一个自愈节拍，否则这条用例只在「恰好没到点」时才有意义。
     */
    TEST(FileWatcher, RemovedRecursiveRootStaysRemovedAcrossSelfHealTicks)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileWatcher_RemoveRecursive");
        const std::unique_ptr<FileWatcher>    watcher = FileWatcher::create();
        ASSERT_NE(watcher, nullptr);

        FileWatchRecorder recorder;
        watcher->setDebounceInterval(std::chrono::milliseconds(0));
        watcher->setCallback([&recorder](const std::string_view filePath, const FileChangeType changeType)
        {
            recorder.record(filePath, changeType);
        });

        ASSERT_TRUE(watcher->addWatch(temporaryDirectory.path().string(), true));
        EXPECT_TRUE(watcher->removeWatch(temporaryDirectory.path().string()));

        ASSERT_TRUE(watcher->start());
        // 跨过至少一个自愈节拍：清单没被摘掉的话，正是这一刻把监视悄悄挂回来
        std::this_thread::sleep_for(std::chrono::milliseconds(2200));
        ASSERT_TRUE(temporaryDirectory.writeFile("should_be_ignored.yaml", "ignored: true"));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        watcher->stop();

        EXPECT_EQ(recorder.eventCount(), 0U) << "撤销掉的递归监视被自愈逻辑重新挂上了，回调仍在派发";
    }

#if ASYN_PLATFORM_LINUX
    /**
     * @brief 钉住（Linux）：被监视目录**改名走开**之后在同一路径重建，再注册一次要真的挂上新目录
     * @details inotify 对「被监视目录本身被改名」只发 IN_MOVE_SELF、不发 IN_IGNORED，内核仍持有该
     *          wd，于是映射不会自己消失；随后的 addWatch(原路径) 撞上「已经看过这个路径」这条去重、
     *          **返回 true 却不注册**，原地重建的同名目录里的变更从此永久丢失。这里刻意不调
     *          removeWatch，走的就是调用方以为「重复注册是幂等的」那条路。
     * @note 只在 Linux 编译：Windows 有同一形状的症状（改名走开再原地重建后收不到事件），但一次
     *       「按句柄实际落处判陈旧、命中则重新注册」的修法在该用例下仍未投递事件，成因尚未定位——
     *       不写没被验证过的结论，Windows 侧留作单独一项。
     */
    TEST(FileWatcher, ReaddedWatchAfterRenameTracksTheNewDirectory)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileWatcher_RenameReadd");
        const std::string                     watchedPath = temporaryDirectory.path().string();

        const std::unique_ptr<FileWatcher> watcher = FileWatcher::create();
        ASSERT_NE(watcher, nullptr);

        FileWatchRecorder recorder;
        watcher->setDebounceInterval(std::chrono::milliseconds(0));
        watcher->setCallback([&recorder](const std::string_view filePath, const FileChangeType changeType)
        {
            recorder.record(filePath, changeType);
        });

        ASSERT_TRUE(watcher->addWatch(watchedPath));
        ASSERT_TRUE(watcher->start());
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        // 把整个被监视目录改名走开，再在**原路径**上建一个同名新目录（全程不 stop、不 removeWatch）
        const std::filesystem::path movedAwayPath =
                temporaryDirectory.path().parent_path() / (temporaryDirectory.path().filename().string() + ".moved");
        std::error_code renameError;
        std::filesystem::rename(temporaryDirectory.path(), movedAwayPath, renameError);
        ASSERT_FALSE(static_cast<bool>(renameError)) << "改名走开失败：" << renameError.message();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ASSERT_TRUE(std::filesystem::create_directories(temporaryDirectory.path()));

        ASSERT_TRUE(watcher->addWatch(watchedPath)) << "重复注册本身要成功（它应当挂的是新目录）";
        std::this_thread::sleep_for(std::chrono::milliseconds(1600));

        ASSERT_TRUE(temporaryDirectory.writeFile("after_rename.yaml", "back: true\n"));
        const bool receivedEvent = TestSupport::waitForCondition(
                [&recorder]()
                {
                    return recorder.sawFileNamed("after_rename.yaml");
                },
                3000);

        watcher->stop();
        EXPECT_TRUE(receivedEvent) << "改名走开后在同一原路径重建的目录没被重新监视，其内部变更永久丢失";

        // 改名走开的那份留在临时目录的兄弟位置上，用例自建自清：不留残留给下一次运行
        std::error_code cleanupError;
        static_cast<void>(std::filesystem::remove_all(movedAwayPath, cleanupError));
    }

    /**
     * @brief 钉住（Linux）：被内核摘除监视之后，同一路径还能重新挂上监视
     * @details 目录被删时内核自动摘 watch 并补一条 IN_IGNORED。实现若不跟着清映射，重建出来的同名
     *          目录会在 addWatch() 里被「路径已在表里」挡下（该分支直接返回 true，看不出失败），
     *          此后这个目录的事件永久丢失——「配置目录被删掉再放回来」正好撞在这条路径上
     */
    TEST(FileWatcher, RecreatedDirectoryCanBeWatchedAgain)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileWatcher_Recreate");
        const std::string                     watchedPath = temporaryDirectory.path().string();

        const std::unique_ptr<FileWatcher> watcher = FileWatcher::create();
        ASSERT_NE(watcher, nullptr);

        FileWatchRecorder recorder;
        watcher->setDebounceInterval(std::chrono::milliseconds(0));
        watcher->setCallback([&recorder](const std::string_view filePath, const FileChangeType changeType)
        {
            recorder.record(filePath, changeType);
        });

        ASSERT_TRUE(watcher->addWatch(watchedPath));
        ASSERT_TRUE(watcher->start());

        // 删掉被监视的目录：内核摘掉 watch 并补一条 IN_IGNORED（此刻本端应当把映射清掉）
        std::error_code removeError;
        static_cast<void>(std::filesystem::remove_all(temporaryDirectory.path(), removeError));
        ASSERT_FALSE(removeError) << "删除被监视目录失败：" << removeError.message();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // 重建同名目录并重新挂监视：没清映射时这一步是空操作（被 contains 挡下）
        ASSERT_TRUE(std::filesystem::create_directories(temporaryDirectory.path()));
        ASSERT_TRUE(watcher->addWatch(watchedPath));

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ASSERT_TRUE(temporaryDirectory.writeFile("reborn.yaml", "back: true\n"));

        const bool receivedEvent = TestSupport::waitForCondition(
                [&recorder]()
                {
                    return recorder.sawFileNamed("reborn.yaml");
                },
                3000);

        watcher->stop();
        EXPECT_TRUE(receivedEvent) << "重建出来的目录收不到事件：监视没挂上（IN_IGNORED 之后映射没清）";
    }
#endif

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

    /**
     * @brief 递归监听开始**之后**新建的子目录也要被覆盖
     * @details 每个目录各自一条监视（Win32 上是一条 ReadDirectoryChangesW），新建的子目录不补挂
     *          就永远收不到它内部的变更。inotify 侧早就有这条补挂，Win32 侧此前只在注册那一刻
     *          递归一遍，之后新建的目录成了监听盲区
     */
    TEST(FileWatcher, RecursiveWatchCoversSubDirectoriesCreatedLater)
    {
        TestSupport::TemporaryDirectory temporaryDirectory("FileWatcher_LateSubDirectory");

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

        // 监听已经在跑：此刻新建子目录，框架要自己把它补进监听集合
        std::error_code error;
        std::filesystem::create_directories(temporaryDirectory.path() / "late", error);
        ASSERT_FALSE(error);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        {
            std::ofstream lateFile(temporaryDirectory.path() / "late" / "late.yaml");
            lateFile << "late: true\n";
        }

        const bool receivedEvent = TestSupport::waitForCondition(
                [&recorder]()
                {
                    return recorder.sawFileNamed("late.yaml");
                },
                3000);

        watcher->stop();
        EXPECT_TRUE(receivedEvent) << "递归监听开始之后新建的子目录没有被补挂监视";
    }

    /**
     * @brief 递归根被整个换掉（删除后重建）时框架自己把监视补回来
     * @details 部署工具常把整个配置目录删掉再放回来。根上的监听随句柄/内核状态失效，而重建后的
     *          目录没有人会再调 addWatch——实现按秒节拍复查递归根并补挂，这条用例不重新注册、
     *          只等自愈，钉住的就是这条自愈路径
     */
    TEST(FileWatcher, RecursiveRootIsRewatchedAfterDirectoryIsReplaced)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileWatcher_ReplaceRoot");
        const std::string                     watchedPath = temporaryDirectory.path().string();

        const std::unique_ptr<FileWatcher> watcher = FileWatcher::create();
        ASSERT_NE(watcher, nullptr);

        FileWatchRecorder recorder;
        watcher->setDebounceInterval(std::chrono::milliseconds(0));
        watcher->setCallback([&recorder](const std::string_view filePath, const FileChangeType changeType)
        {
            recorder.record(filePath, changeType);
        });

        ASSERT_TRUE(watcher->addWatch(watchedPath, /*recursive=*/true));
        ASSERT_TRUE(watcher->start());
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        // 把整个目录换掉：删掉再重建，**不**重新 addWatch
        std::error_code removeError;
        static_cast<void>(std::filesystem::remove_all(temporaryDirectory.path(), removeError));
        ASSERT_FALSE(removeError) << "删除被监视目录失败：" << removeError.message();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ASSERT_TRUE(std::filesystem::create_directories(temporaryDirectory.path()));

        // 等自愈复查（1 秒节拍）把根重新挂上
        std::this_thread::sleep_for(std::chrono::milliseconds(1600));

        ASSERT_TRUE(temporaryDirectory.writeFile("replaced.yaml", "back: true\n"));

        const bool receivedEvent = TestSupport::waitForCondition(
                [&recorder]()
                {
                    return recorder.sawFileNamed("replaced.yaml");
                },
                3000);

        watcher->stop();
        EXPECT_TRUE(receivedEvent) << "换掉重建的递归根没有被补挂监视：它内部的变更此后永久丢失";
    }

#if ASYN_PLATFORM_WIN32
    /**
     * @brief 钉住（Windows）：监视目录数超过单次等待上限时，尾部目录也要收得到事件
     * @details WaitForMultipleObjects 单次最多等 64 个对象（含停止事件，因此目录只有 63 个额度）。
     *          原实现只截前 63 个、且遍历顺序稳定，排在后面的目录永远进不了等待集——它们的事件
     *          永久丢失。这里递归监视 70 个子目录，写**最后一个**子目录里的文件来钉住轮转切片
     */
    TEST(FileWatcher, WatchesMoreDirectoriesThanOneWaitBatch)
    {
        constexpr int kSubDirectoryCount = 70; // 超过 63 才会触发截断
        TestSupport::TemporaryDirectory temporaryDirectory("FileWatcher_ManyDirectories");
        std::error_code                 error;
        for (int index = 0; index < kSubDirectoryCount; ++index)
        {
            std::filesystem::create_directories(temporaryDirectory.path() / ("sub-" + std::to_string(index)), error);
            ASSERT_FALSE(error) << "建子目录失败：" << error.message();
        }

        const std::unique_ptr<FileWatcher> watcher = FileWatcher::create();
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

        // 写在最靠后的子目录里：只截前 63 个的实现恰好把它排在等待集之外
        {
            std::ofstream tailFile(temporaryDirectory.path() / ("sub-" + std::to_string(kSubDirectoryCount - 1)) / "tail.yaml");
            tailFile << "tail: true\n";
        }

        const bool receivedEvent = TestSupport::waitForCondition(
                [&recorder]()
                {
                    return recorder.sawFileNamed("tail.yaml");
                },
                5000);

        watcher->stop();
        EXPECT_TRUE(receivedEvent) << "排在等待批次之外的目录收不到事件：批次没有轮转";
    }
#endif

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
