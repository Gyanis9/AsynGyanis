/**
 * @file DatabaseTestSupport.h
 * @brief Database 单元测试辅助：临时数据库文件夹具、唯一路径生成与协程测试驱动
 * @details 协程部分（waitForCondition / CompletedTask / collectTask / EventLoopThread）是
 *          Database 异步 API 的公共测试设施，SQLite 与真实服务端的用例共用同一套纪律，
 *          本文件是它唯一的落点；因此本头只依赖 Core/EventLoop/EventLoop.h、
 *          Core/Coroutine/Task.h 与标准库，不引入任何 Database 驱动头。
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Platform/Platform.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace AsynGyanis::Database::TestSupport
{
    /**
     * @brief 读一个环境变量并拷贝成 std::string
     * @details MSVC 在 /W4 下把 std::getenv 判为弃用（C4996），Windows 侧改用 _dupenv_s；
     *          两种实现的返回值都立即拷贝，调用方不保留任何指向环境块的指针
     * @param variableName 环境变量名
     * @return std::string 取值；未设置时为空串
     */
    inline std::string readEnvironmentVariableText(const char *variableName)
    {
#if ASYN_PLATFORM_WIN32
        char  *rawValue   = nullptr;
        size_t valueCapacity = 0;
        if (::_dupenv_s(&rawValue, &valueCapacity, variableName) != 0 || rawValue == nullptr)
        {
            return {};
        }
        std::string variableValue(rawValue);
        std::free(rawValue);
        return variableValue;
#else
        const char *rawValue = std::getenv(variableName);
        return rawValue != nullptr ? std::string(rawValue) : std::string{};
#endif
    }

    /**
     * @brief 生成一个进程内、跨进程都唯一的名字，用作临时数据库文件的主干
     * @details 三重盐值缺一不可：steady_clock 读数隔开不同时刻创建的用例，静态自增序号隔开同一
     *          纳秒内连续创建的用例，random_device 隔开并发运行的测试进程——gtest_discover_tests
     *          会给每个用例单独起进程，仅靠进程内序号无法避免两个进程同时算出同一个名字。
     * @param namePrefix 便于定位问题的用途前缀，如 "SqliteConnect"
     * @return std::string 以 AsynGyanis_Database_ 开头的名字，不含目录、不含扩展名
     */
    inline std::string makeUniqueDatabaseName(const std::string &namePrefix)
    {
        static std::atomic<unsigned int> sequenceCounter{0};

        const std::string clockSalt     = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const std::string sequenceSalt  = std::to_string(sequenceCounter.fetch_add(1));
        const std::string entropySalt   = std::to_string(std::random_device{}());

        return "AsynGyanis_Database_" + namePrefix + "_" + clockSalt + "_" + sequenceSalt + "_" + entropySalt;
    }

    /**
     * @brief 把平台路径转成 SQLite C API 要求的 UTF-8 窄字节序列
     * @details 不用 path::string()：它在 Windows 上按进程 ANSI 代码页转换，用户名含非 ASCII 字符时结果有损，
     *          且 MSVC 已将其标记为弃用；u8string() 在两个平台都稳定产出 UTF-8，
     *          而 sqlite3_open() 的入参约定正是 UTF-8。
     * @param databasePath 待转换的路径
     * @return std::string 原始 UTF-8 字节，逐字节等于 u8string() 的内容
     */
    inline std::string toUtf8PathText(const std::filesystem::path &databasePath)
    {
        const std::u8string utf8Name = databasePath.u8string();

        // char8_t 与 char 布局一致，这里只换类型不改字节，避免任何编码转换
        return std::string(reinterpret_cast<const char *>(utf8Name.data()), utf8Name.size());
    }

    /**
     * @brief 临时数据库文件夹具
     *
     * @details 在系统临时目录下拼出一个进程内唯一的 *.db 路径，析构时连同同名伴生文件
     *          （WAL 的 -wal / -shm、回滚日志 -journal）一并删除。构造阶段刻意不做文件 IO：
     *          文件本体由被测的 connect() 创建。使用要求：连接必须在本对象之后创建、先于本对象
     *          析构，否则 Windows 上文件仍被占用，删除会静默失败（POSIX 下能删掉但句柄未释放）。
     */
    class TemporaryDatabaseFile
    {
    public:
        /**
         * @brief 拼接临时目录下的唯一数据库路径，不创建文件
         * @param namePrefix 便于定位问题的用途前缀，如 "SqliteWal"
         */
        explicit TemporaryDatabaseFile(const std::string &namePrefix)
            : m_path(std::filesystem::temp_directory_path() / (makeUniqueDatabaseName(namePrefix) + ".db"))
        {
        }

        /**
         * @brief 析构并删除数据库文件与全部同名伴生文件，删除失败一律忽略
         */
        ~TemporaryDatabaseFile()
        {
            removeDatabaseAndSidecarFiles();
        }

        TemporaryDatabaseFile(const TemporaryDatabaseFile &)            = delete;
        TemporaryDatabaseFile &operator=(const TemporaryDatabaseFile &) = delete;

        /**
         * @brief 获取数据库文件路径
         * @return const std::filesystem::path& 临时目录下的绝对路径
         */
        [[nodiscard]] const std::filesystem::path &path() const noexcept
        {
            return m_path;
        }

        /**
         * @brief 获取可直接交给 ConnectionConfig::database 的 UTF-8 路径文本
         * @return std::string 平台无关的 UTF-8 字节序列
         */
        [[nodiscard]] std::string utf8Path() const
        {
            return toUtf8PathText(m_path);
        }

        /**
         * @brief 判断数据库文件是否已落盘
         * @return true 文件存在
         */
        [[nodiscard]] bool exists() const
        {
            std::error_code error;
            return std::filesystem::exists(m_path, error);
        }

        /**
         * @brief 获取数据库文件字节数
         * @details 文件不存在或读取失败时返回 0，因此「大小大于 0」同时隐含了「文件已存在」。
         * @return std::uintmax_t 主数据库文件大小，不含 WAL 伴生文件
         */
        [[nodiscard]] std::uintmax_t fileSizeBytes() const
        {
            std::error_code error;
            const std::uintmax_t fileSize = std::filesystem::file_size(m_path, error);
            return error ? 0 : fileSize;
        }

    private:
        /**
         * @brief 删除主文件与全部同名伴生文件，逐个忽略错误
         */
        void removeDatabaseAndSidecarFiles() const
        {
            // WAL 模式留下 -wal 与 -shm，回滚日志模式留下 -journal；正常收尾时 SQLite 会自己删掉，
            // 但用例中途失败、句柄尚未释放或进程被杀都会残留下来，一律按同名同后缀补删一次
            static constexpr std::array<const char *, 3> kSidecarSuffixes{"-wal", "-shm", "-journal"};

            std::error_code error;
            std::filesystem::remove(m_path, error);

            for (const char *sidecarSuffix: kSidecarSuffixes)
            {
                // 后缀只含 ASCII，用 path::operator+= 直接追加，避免再走一次窄字符到平台路径的编码转换
                std::filesystem::path sidecarPath = m_path;
                sidecarPath += sidecarSuffix;
                std::filesystem::remove(sidecarPath, error);
            }
        }

        std::filesystem::path m_path; ///< 临时数据库文件绝对路径
    };

    // ========================================================================
    // 协程驱动辅助
    // ========================================================================

    /// 轮询等待的上限：所有用例的正常耗时都在毫秒级，5 秒足够暴露「协程没被恢复」这类问题
    inline constexpr std::chrono::milliseconds kWaitTimeout{5000};

    /**
     * @brief 轮询等待条件成立（避免固定 sleep 造成的偶发失败）
     * @tparam Predicate 判定可调用对象
     * @param predicate 判定函数
     * @return true 条件在时限内成立
     */
    template<typename Predicate>
    [[nodiscard]] bool waitForCondition(Predicate predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (!predicate())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    /**
     * @brief 一次异步任务的观测结果
     * @tparam ResultType 任务结果类型
     */
    template<typename ResultType>
    struct CompletedTask
    {
        std::optional<ResultType> value;            ///< 任务返回值（成功时才有值）
        std::exception_ptr        error;            ///< 任务抛出的异常（失败时非空）
        bool                      finished = false; ///< 是否在时限内收到完成通知
    };

    /**
     * @brief 驱动协程：co_await 目标任务，把结果或异常搬进调用方提供的变量
     *
     * @details 之所以需要这层驱动，是因为 Task<T> 只能被协程 co_await：
     *          本协程先挂起在内层任务上，内层完成后（在事件循环线程上）恢复，
     *          这里把结果写出去并最后置完成标记——标记由事件循环线程写入，
     *          调用线程读取前必须做 acquire 语义的同步（见 std::atomic 内存序）。
     *
     * @tparam ResultType 内层任务的结果类型
     * @param inner 待等待的任务（按值接收，帧内持有它的生命周期）
     * @param value 出参：任务返回值
     * @param error 出参：任务抛出的异常
     * @param finished 出参：完成标记，写于所有其它出参之后
     * @return Core::Task<void> 驱动协程
     */
    template<typename ResultType>
    Core::Task<void> collectTask(Core::Task<ResultType> inner,
                                std::optional<ResultType> &value,
                                std::exception_ptr &error,
                                std::atomic<bool> &finished)
    {
        try
        {
            value.emplace(co_await std::move(inner));
        }
        catch (...)
        {
            // 任务异常在此收敛：驱动协程本身不向上抛，调用线程只需看 error 是否被写入
            error = std::current_exception();
        }

        // release 语义：保证上面的写入对读取到本标记的线程可见
        finished.store(true, std::memory_order_release);
    }

    /**
     * @brief 后台事件循环运行器：构造即起线程跑 EventLoop::run()，析构先 stop() 再 join
     *
     * @details 异步 API 的协程由调用线程 inline 启动、由事件循环线程恢复，本类把「正在运行的
     *          EventLoop」与「搬出 Task 结果的驱动协程」放在一起并承担销毁纪律：**协程帧必须活到
     *          事件循环线程结束之后**——驱动协程帧一律留在 m_driverTasks，且它声明在 m_thread
     *          **之前**，逆序析构保证帧销毁晚于循环线程结束；调用方自持的驱动协程对象须声明在本类之前。
     */
    class EventLoopThread
    {
    public:
        EventLoopThread()
            : m_thread([this]()
            {
                m_loop.run();
            })
        {
        }

        /**
         * @brief 析构：先请求停止事件循环，join 由 m_thread 成员析构完成
         * @details stop() 只是置停止标志并唤醒 epoll_wait，因此必须等到 join 之后才算真正
         *          「循环线程已结束」——这正是 m_thread 成员排在 m_driverTasks 之前的意义。
         */
        ~EventLoopThread()
        {
            m_loop.stop();
        }

        EventLoopThread(const EventLoopThread &)            = delete;
        EventLoopThread &operator=(const EventLoopThread &) = delete;

        /**
         * @brief 获取后台线程上运行的事件循环
         * @return Core::EventLoop& 事件循环，供提交协程、注册事件使用
         */
        [[nodiscard]] Core::EventLoop &loop() noexcept
        {
            return m_loop;
        }

        /**
         * @brief 事件循环的 run() 是否已经进入循环体
         * @return true 已进入；false 尚未启动或已停止
         */
        [[nodiscard]] bool isRunning() const noexcept
        {
            return m_loop.isRunning();
        }

        /**
         * @brief 取得后台循环线程的 id
         * @details 用于断言「协程恢复发生在事件循环线程上」这类线程归属性质：
         *          在驱动协程里记录 `std::this_thread::get_id()`，再与本方法比较即可。
         * @return std::thread::id 循环线程 id；线程尚未启动时为空 id
         */
        [[nodiscard]] std::thread::id threadId() const noexcept
        {
            return m_thread.get_id();
        }

        /**
         * @brief 自旋等待事件循环进入运行状态
         * @details 线程刚启动时 run() 可能还没读到运行标志，此时提交的协程虽然不会丢
         *          （调度器会先入队），但断言「已经跑起来」会让用例结论依赖调度时序。
         *          需要确定性起点的用例调用本方法等一个明确的「已运行」。
         * @return true 在 kWaitTimeout 内进入运行状态
         */
        [[nodiscard]] bool waitUntilRunning() const
        {
            return waitForCondition([this]()
            {
                return m_loop.isRunning();
            });
        }

        /**
         * @brief 启动一个任务并等到它完成
         *
         * @details 以 resume 驱动协程启动：内层任务内联执行到「把阻塞任务交给执行器」这一步
         *          就挂起，因此调用线程不会被占住等待数据库，由执行器与循环线程协作推进。
         *          完成标记是驱动协程的**最后一次**写入（此后只走 final_suspend 收尾、不再触碰
         *          出参），故按值搬出结果安全；帧本身仍留在 m_driverTasks 里活到 join 之后。
         *
         * @tparam ResultType 任务结果类型
         * @param task 待执行的异步任务
         * @return CompletedTask<ResultType> 结果、异常与完成情况
         */
        template<typename ResultType>
        [[nodiscard]] CompletedTask<ResultType> runToCompletion(Core::Task<ResultType> task)
        {
            CompletedTask<ResultType> completed;
            std::atomic<bool>         finishedFlag{false};

            Core::Task<void> driver =
                collectTask<ResultType>(std::move(task), completed.value, completed.error, finishedFlag);
            // 内联启动：阻塞任务只做入队，控制权在这里立刻回到调用线程
            driver.handle().resume();

            completed.finished = waitForCondition([&finishedFlag]()
            {
                return finishedFlag.load(std::memory_order_acquire);
            });

            // 帧的销毁推迟到事件循环线程 join 之后（见类注释的成员声明顺序）
            m_driverTasks.push_back(std::move(driver));
            return completed;
        }

        /**
         * @brief 把调用方自己的驱动协程交给运行器保管，直到循环线程结束
         *
         * @details 用于「提交后先断言尚未完成、放行后再等结果」这类用例：此时协程帧不能放在
         *          用例体的局部对象里（用例体先于夹具成员析构，帧会与 resume() 收尾竞态），
         *          交出来让运行器按成员声明顺序统一销毁即可。
         * @param driver 待保管的驱动协程（通常由 collectTask() 产出）
         */
        void parkDriver(Core::Task<void> driver)
        {
            m_driverTasks.push_back(std::move(driver));
        }

    private:
        Core::EventLoop               m_loop;        ///< 后台线程上运行的事件循环
        std::vector<Core::Task<void>> m_driverTasks; ///< 驱动协程：声明在 m_thread 之前，故晚于 join 销毁
        std::jthread                  m_thread;      ///< 跑 m_loop.run() 的后台线程，析构自动 join
    };

} // namespace AsynGyanis::Database::TestSupport
