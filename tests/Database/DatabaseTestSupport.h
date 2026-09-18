/**
 * @file DatabaseTestSupport.h
 * @brief Database 单元测试辅助：临时数据库文件夹具、唯一路径生成与查询构建助手
 * @details 协程驱动设施（等待、CompletedTask/collectTask、EventLoopThread）统一定义在
 *          CoreTestSupport.h，本文件只做转发，SQLite 与真实服务端的异步用例因此与 Core/Net
 *          共用同一套纪律；本头只依赖 Common/Queryable 下的接口头（没有任何驱动头）、
 *          Core/Platform 与标准库，驱动被编成桩时照样可用。
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "CoreTestSupport.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Queryable/QueryNode.h"
#include "Platform/Platform.h"
#include "Platform/System/ProcessInfo.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace AsynGyanis::Database::TestSupport
{
    /**
     * @brief 读一个环境变量并拷贝成 std::string
     * @details 平台差异（MSVC 的 C4996 与内存归属）统一由 Platform::ProcessInfo 承担，
     *          这里只把 optional 折成用例口径的「未设置即空串」。
     * @param variableName 环境变量名
     * @return std::string 取值；未设置时为空串
     */
    inline std::string readEnvironmentVariableText(const std::string &variableName)
    {
        return Platform::ProcessInfo::environmentVariable(variableName).value_or(std::string{});
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
     * @details 在系统临时目录下拼出一个进程内唯一的 *.db 路径，析构时连同伴生文件（-wal / -shm / -journal）一并删除；
     *          构造阶段刻意不做文件 IO，文件本体由被测的 connect() 创建。使用要求：连接必须在本对象之后创建、
     *          先于本对象析构，否则 Windows 上文件仍被占用，删除会静默失败（POSIX 下能删掉但句柄未释放）。
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
    // 协程驱动辅助（定义在 CoreTestSupport.h，此处转发给 Database 用例）
    // ========================================================================

    /// 等待类断言的统一上限
    using AsynGyanis::Core::TestSupport::kWaitTimeout;

    /// 在时限内轮询等待条件成立
    using AsynGyanis::Core::TestSupport::waitForCondition;

    /// 一次异步任务的观测结果（结果值、异常与完成情况）
    using AsynGyanis::Core::TestSupport::CompletedTask;

    /// 驱动协程：co_await 目标任务，把结果或异常搬进调用方提供的变量
    using AsynGyanis::Core::TestSupport::collectTask;

    /// 后台事件循环运行器（自持循环模式，销毁纪律见其类注释）
    using AsynGyanis::Core::TestSupport::EventLoopThread;

    /**
     * @brief 判断文本是否含非 ASCII 字节，即「驱动自己拼了中文说明」的稳定判据
     * @details 断言只查「有中文 + 有底层关键英文原文 + 有错误码」，不硬编码整句中文，
     *          免得底层库升级改了英文措辞时用例集体失败。
     * @param text 待判定文本
     * @return true 至少有一个字节的最高位被置起
     */
    [[nodiscard]] inline bool containsLocalizedText(const std::string &text)
    {
        return std::any_of(text.begin(), text.end(),
                           [](const char character) { return static_cast<unsigned char>(character) >= 0x80; });
    }

    /**
     * @brief 读取文本型环境变量，未设置时回落到默认值
     * @param variableName 环境变量名
     * @param fallback 未设置时使用的默认值
     * @return std::string 生效取值
     */
    [[nodiscard]] inline std::string readEnvironmentTextOrDefault(const std::string &variableName, const std::string_view fallback)
    {
        const std::string variableValue = readEnvironmentVariableText(variableName);
        return variableValue.empty() ? std::string(fallback) : variableValue;
    }

    /**
     * @brief 读取端口型环境变量，未设置或取值非法时回落默认端口
     * @details 用 std::from_chars 而不是 std::stoi：后者靠异常报错且接受 "3306abc" 这类带余文的输入。
     *          非法取值一律回落，不因为环境写错就让整组用例失败。
     * @param variableName 环境变量名
     * @param fallback 未设置或取值非法时使用的默认端口
     * @return std::uint16_t 生效端口
     */
    [[nodiscard]] inline std::uint16_t readEnvironmentPortOrDefault(const std::string &variableName, const std::uint16_t fallback)
    {
        const std::string portText = readEnvironmentVariableText(variableName);
        if (portText.empty())
        {
            return fallback;
        }

        int        parsedPort = 0;
        const auto [remainderBegin, parseError] = std::from_chars(portText.data(), portText.data() + portText.size(), parsedPort);

        // 三种非法情形一律回落：解析失败、尾部有余文、超出 1..65535 的端口范围
        if (parseError != std::errc{} || remainderBegin != portText.data() + portText.size() || parsedPort <= 0 || parsedPort > 65535)
        {
            return fallback;
        }

        return static_cast<std::uint16_t>(parsedPort);
    }

    // ========================================================================
    // 查询构建与结果读取助手（方言测试与 SQLite 测试共用）
    // ========================================================================

    /**
     * @brief 统计 SQL 文本里的占位符个数
     * @param sql 待统计的 SQL 文本
     * @return std::size_t '?' 出现的次数
     */
    [[nodiscard]] inline std::size_t countPlaceholders(const std::string &sql)
    {
        std::size_t count = 0;
        for (const char character: sql)
        {
            if (character == '?')
            {
                ++count;
            }
        }
        return count;
    }

    /**
     * @brief 构造字段引用
     * @param name 列名或表达式文本
     * @return Queryable::FieldReference 字段引用
     */
    [[nodiscard]] inline Queryable::FieldReference makeField(const std::string &name)
    {
        return Queryable::FieldReference{.name = name};
    }

    /**
     * @brief 构造 列 与 参数值 的比较条件
     * @param columnName 列名
     * @param sqlOperator 比较操作符
     * @param value 右操作数参数值
     * @return Queryable::WhereCondition 条件节点
     */
    [[nodiscard]] inline Queryable::WhereCondition makeComparison(const std::string &columnName,
                                                                  const Queryable::SqlOperator sqlOperator,
                                                                  const Queryable::ParameterValue &value)
    {
        return Queryable::WhereCondition{
            .left  = makeField(columnName),
            .op    = sqlOperator,
            .right = value
        };
    }

    /**
     * @brief 构造 列 与 列 的比较条件（右操作数是字段引用）
     * @param columnName 左列名
     * @param sqlOperator 比较操作符
     * @param rightColumnName 右列名
     * @return Queryable::WhereCondition 条件节点
     */
    [[nodiscard]] inline Queryable::WhereCondition makeColumnComparison(const std::string &columnName,
                                                                       const Queryable::SqlOperator sqlOperator,
                                                                       const std::string &rightColumnName)
    {
        return Queryable::WhereCondition{
            .left  = makeField(columnName),
            .op    = sqlOperator,
            .right = makeField(rightColumnName)
        };
    }

    /**
     * @brief 构造由 children 组成的复合条件
     * @param sqlOperator And / Or / Not
     * @param children 子条件列表
     * @return Queryable::WhereCondition 复合条件节点
     */
    [[nodiscard]] inline Queryable::WhereCondition makeComposite(const Queryable::SqlOperator sqlOperator,
                                                                std::vector<Queryable::WhereCondition> children)
    {
        Queryable::WhereCondition condition;
        condition.op       = sqlOperator;
        condition.right    = Queryable::ParameterValue{nullptr};
        condition.children = std::move(children);
        return condition;
    }

    /**
     * @brief 构造 IN / NOT IN 条件
     * @param columnName 列名
     * @param sqlOperator In 或 NotIn
     * @param values 值集合
     * @return Queryable::WhereCondition 条件节点
     */
    [[nodiscard]] inline Queryable::WhereCondition makeInCondition(const std::string &columnName,
                                                                  const Queryable::SqlOperator sqlOperator,
                                                                  std::vector<Queryable::ParameterValue> values)
    {
        Queryable::WhereCondition condition;
        condition.left     = makeField(columnName);
        condition.op       = sqlOperator;
        condition.right    = Queryable::ParameterValue{static_cast<std::int64_t>(0)};
        condition.inValues = std::move(values);
        return condition;
    }

    /**
     * @brief 执行一条按契约应当成功的命令
     * @details 用 EXPECT 记录失败，返回的指针仍可能为空，调用方自行 ASSERT_NE 决定是否中止。
     * @param connection 已连接的数据库连接
     * @param command SQL 文本
     * @return std::unique_ptr<DatabaseResult> 结果集；失败时为空
     */
    [[nodiscard]] inline std::unique_ptr<DatabaseResult> executeRequired(DatabaseConnection &connection, const std::string_view command)
    {
        std::unique_ptr<DatabaseResult> result = connection.execute(command);
        EXPECT_NE(result, nullptr) << "命令本应执行成功：" << command << "，原因：" << connection.lastError();
        return result;
    }

    /**
     * @brief 安全取出整型列值
     * @param value 待判定的数据库值
     * @return std::optional<std::int64_t> 类型不符时返回空值而不是抛异常
     */
    [[nodiscard]] inline std::optional<std::int64_t> asInteger(const DatabaseValue &value)
    {
        const auto *integer = std::get_if<std::int64_t>(&value);
        return integer == nullptr ? std::nullopt : std::optional<std::int64_t>(*integer);
    }

    /**
     * @brief 安全取出文本列值
     * @param value 待判定的数据库值
     * @return std::optional<std::string> 类型不符时返回空值
     */
    [[nodiscard]] inline std::optional<std::string> asText(const DatabaseValue &value)
    {
        const auto *text = std::get_if<std::string>(&value);
        return text == nullptr ? std::nullopt : std::optional<std::string>(*text);
    }
} // namespace AsynGyanis::Database::TestSupport
