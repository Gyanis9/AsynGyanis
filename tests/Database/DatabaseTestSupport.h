/**
 * @file DatabaseTestSupport.h
 * @brief Database 单元测试辅助：临时 SQLite 数据库文件夹具与唯一路径生成
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>

namespace AsynGyanis::Database::TestSupport
{
    /**
     * @brief 生成一个进程内、跨进程都唯一的名字，用作临时数据库文件的主干
     * @details 三重盐值缺一不可：
     *          - steady_clock 读数隔开不同时刻创建的用例；
     *          - 静态自增序号隔开同一纳秒读数内连续创建的用例（同进程内绝对不重名）；
     *          - random_device 隔开并发运行的不同测试进程：gtest_discover_tests 会给每个用例单独起进程，
     *            仅靠进程内序号无法避免两个进程同时算出同一个名字。
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
     * @details 在系统临时目录下拼出一个进程内唯一的 *.db 路径，析构时删除该文件以及同名伴生文件
     *          （WAL 的 -wal / -shm、回滚日志的 -journal），使每个用例既互不干扰也不留残留。
     *          构造阶段刻意不做任何文件 IO：文件本体由被测的 connect() 创建，
     *          「连接前文件不存在、连接后存在」本身就是一条要钉住的契约。
     *
     * 使用要求：数据库路径必须在本对象之后创建连接，并保证连接先于本对象析构，
     *          否则 Windows 上文件仍被占用，删除会静默失败（POSIX 下能删掉但句柄未释放）。
     *
     * @code
     *   TestSupport::TemporaryDatabaseFile databaseFile("SqlitePersistence");
     *   ConnectionConfig configuration = ConnectionConfig::sqliteDefault(databaseFile.utf8Path());
     * @endcode
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

} // namespace AsynGyanis::Database::TestSupport
