// Database 子系统自检：SQLite 文件库与内存库、方言翻译、ORM 建表与增删改查、事务提交与回滚、二进制列逐字节往返、
// 参数绑定与约束违例、连接池取还与等待交接、异步执行链路，以及按环境变量门控的 MySQL / Redis 真机
#include "Base/Exception/Exception.h"
#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Platform/System/ProcessInfo.h"

#include "Database/Common/BinaryBytes.h"
#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseException.h"
#include "Database/Common/DatabaseFactory.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseType.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Common/QueryExecutionException.h"
#include "Database/Dialect/ColumnType.h"
#include "Database/Dialect/DialectRegistry.h"
#include "Database/Dialect/MySqlDialect.h"
#include "Database/Dialect/SqlDialect.h"
#include "Database/Dialect/SqliteDialect.h"
#include "Database/MySql/MySqlConnection.h"
#include "Database/Pool/AsyncExecutor.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"
#include "Database/Pool/PooledConnection.h"
#include "Database/Pool/Transaction.h"
#include "Database/Queryable/Column.h"
#include "Database/Queryable/Expression.h"
#include "Database/Queryable/Queryable.h"
#include "Database/Queryable/SchemaMigrator.h"
#include "Database/Queryable/TableSchema.h"
#include "Database/Redis/RedisConnection.h"
#include "Database/Sqlite/SqliteConnection.h"
#include "Database/Sqlite/SqliteResult.h"
#include "common/SampleSupport.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace AsynGyanis;

namespace
{
    /**
     * @brief 账户表：整型主键 + 文本 + 浮点 + 可空文本 + 布尔位，覆盖 ORM 的全部标量映射分支
     */
    struct AccountRow
    {
        std::int64_t               id;      ///< 主键
        std::string                name;    ///< 户名（含中文与注入样本）
        double                     balance; ///< 余额（含负数）
        std::optional<std::string> note;    ///< 备注：SQL NULL 落成空 optional
        bool                       active;  ///< 是否启用
    };

    /**
     * @brief 二进制表：两种成员拼法各一列，另加一列可空二进制，用来区分「零长载荷」与「NULL」
     */
    struct DocumentRow
    {
        std::int64_t                         id;         ///< 主键
        Database::BinaryBytes                payload;    ///< 二进制列（规范拼法）
        std::vector<std::byte>               rawPayload; ///< 二进制列（等价拼法）
        std::optional<Database::BinaryBytes> note;       ///< 可空二进制列
    };

    // 表名与列名刻意含空格：只有方言把标识符整段引用起来这条路才走得通，顺带钉住引用实现
    constexpr std::string_view kAccountTableName = "sample accounts";
    constexpr std::string_view kDocumentTableName = "sample documents";

    /**
     * @brief 原生 SQL 示范用的表：整数主键 + 文本 + 浮点 + 可空整数 + 二进制
     */
    constexpr std::string_view kCreateRawTableSql =
            "CREATE TABLE raw_values ("
            " id INTEGER PRIMARY KEY,"
            " label TEXT NOT NULL,"
            " ratio REAL,"
            " age INTEGER,"
            " payload BLOB)";

    /// 环境变量的缺省取值与变量名一律与 tests/Database 下同名约定保持一致：凭据只从环境进来
    constexpr const char *kMySqlHostVariableName     = "ASYN_MYSQL_TEST_HOST";
    constexpr const char *kMySqlPortVariableName     = "ASYN_MYSQL_TEST_PORT";
    constexpr const char *kMySqlUserVariableName     = "ASYN_MYSQL_TEST_USER";
    constexpr const char *kMySqlPasswordVariableName = "ASYN_MYSQL_TEST_PASSWORD";
    constexpr const char *kMySqlDatabaseVariableName = "ASYN_MYSQL_TEST_DATABASE";

    constexpr const char *kRedisHostVariableName     = "ASYN_REDIS_TEST_HOST";
    constexpr const char *kRedisPortVariableName     = "ASYN_REDIS_TEST_PORT";
    constexpr const char *kRedisUserVariableName     = "ASYN_REDIS_TEST_USER";
    constexpr const char *kRedisPasswordVariableName = "ASYN_REDIS_TEST_PASSWORD";
    constexpr const char *kRedisDatabaseVariableName = "ASYN_REDIS_TEST_DATABASE";

    constexpr const char *kDefaultMySqlHost         = "127.0.0.1";
    constexpr std::uint16_t kDefaultMySqlPort       = 3306U;
    constexpr const char *kDefaultMySqlUserName     = "root";
    constexpr const char *kDefaultMySqlDatabaseName = "asyngyanis_test";

    constexpr const char *kDefaultRedisHost       = "127.0.0.1";
    constexpr std::uint16_t kDefaultRedisPort     = 6379U;
    // 键空间刻意不用 0（免得混进使用者的工作库），文本与编号两份取值必须一致
    constexpr std::string_view kDefaultRedisKeyspace      = "15";
    constexpr int              kDefaultRedisKeyspaceIndex = 15;

#ifdef DATABASE_HAS_MYSQL
    constexpr bool kMySqlDriverCompiled = true;
#else
    constexpr bool kMySqlDriverCompiled = false;
#endif

#ifdef DATABASE_HAS_REDIS
    constexpr bool kRedisDriverCompiled = true;
#else
    constexpr bool kRedisDriverCompiled = false;
#endif
} // namespace

// ========================================================================
// TableSchema 特化：表名、列与主键的唯一定义处
// 必须写在用到 Queryable<T> 的任何代码之前：特化晚于使用点会让编译器按主模板（空列）实例化
// ========================================================================

template<>
struct AsynGyanis::Database::Queryable::TableSchema<AccountRow>
{
    static constexpr std::string_view kTableName = kAccountTableName;
    static constexpr auto kColumns = std::tuple{
        Column(&AccountRow::id, "id"),
        Column(&AccountRow::name, "name"),
        Column(&AccountRow::balance, "balance"),
        Column(&AccountRow::note, "note"),
        Column(&AccountRow::active, "active"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

template<>
struct AsynGyanis::Database::Queryable::TableSchema<DocumentRow>
{
    static constexpr std::string_view kTableName = kDocumentTableName;
    static constexpr auto kColumns = std::tuple{
        Column(&DocumentRow::id, "id"),
        Column(&DocumentRow::payload, "payload"),
        Column(&DocumentRow::rawPayload, "raw_payload"),
        Column(&DocumentRow::note, "note"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

namespace
{
    /**
     * @brief ORM 查询对象的别名：全名 Database::Queryable::Queryable<T> 在正文里太长
     * @tparam RowType 表数据结构类型
     */
    template<typename RowType>
    using OrmQuery = Database::Queryable::Queryable<RowType>;

    // 条件表达式里反复按列名构造描述符，集中一处写名字，避免与 TableSchema 的拼写漂移
    constexpr auto kAccountIdColumn          = Database::Queryable::Column(&AccountRow::id, "id");
    constexpr auto kAccountNameColumn        = Database::Queryable::Column(&AccountRow::name, "name");
    constexpr auto kAccountBalanceColumn     = Database::Queryable::Column(&AccountRow::balance, "balance");
    constexpr auto kAccountActiveColumn      = Database::Queryable::Column(&AccountRow::active, "active");
    constexpr auto kDocumentPayloadColumn    = Database::Queryable::Column(&DocumentRow::payload, "payload");

    /// 把平台路径转成 SQLite C API 要求的 UTF-8 窄字节（path::string() 在 Windows 上按 ANSI 代码页转换，有损）
    std::string toUtf8PathText(const std::filesystem::path &databasePath)
    {
        const std::u8string utf8Name = databasePath.u8string();
        // char8_t 与 char 布局一致，这里只换类型不改字节
        return std::string(reinterpret_cast<const char *>(utf8Name.data()), utf8Name.size());
    }

    /**
     * @brief 临时数据库文件：析构时连 WAL / journal 伴生文件一起删掉
     * @details 连接必须先于本对象销毁，否则 Windows 上文件仍被占用，删除会静默失败
     */
    class TemporaryDatabaseFile
    {
    public:
        /// @param purpose 用途标记，进文件名便于定位是哪一步留下的文件
        explicit TemporaryDatabaseFile(const std::string_view purpose)
            : m_path(std::filesystem::temp_directory_path() /
                     ("asyn-sample-database-" + std::string(purpose) + "-" +
                      std::to_string(Platform::ProcessInfo::currentProcessId()) + ".db"))
        {
            // 上一次崩溃留下的残骸（含伴生文件）先清干净，否则「connect() 之前文件不存在」这条判据会被污染
            removeWithSidecars();
        }

        ~TemporaryDatabaseFile()
        {
            removeWithSidecars();
        }

        TemporaryDatabaseFile(const TemporaryDatabaseFile &)            = delete;
        TemporaryDatabaseFile &operator=(const TemporaryDatabaseFile &) = delete;

        [[nodiscard]] bool exists() const
        {
            std::error_code ignored;
            return std::filesystem::exists(m_path, ignored);
        }

        [[nodiscard]] std::uintmax_t fileSizeBytes() const
        {
            std::error_code ignored;
            const std::uintmax_t fileSize = std::filesystem::file_size(m_path, ignored);
            return ignored ? 0 : fileSize;
        }

        /// @return std::string 可直接交给 ConnectionConfig::database 的 UTF-8 路径文本
        [[nodiscard]] std::string utf8Path() const
        {
            return toUtf8PathText(m_path);
        }

    private:
        /// 删主文件与全部同名伴生文件：WAL 留 -wal / -shm，回滚日志留 -journal，逐个忽略错误
        void removeWithSidecars() const
        {
            std::error_code ignored;
            std::filesystem::remove(m_path, ignored);
            for (const std::string_view suffix: {std::string_view{"-wal"}, std::string_view{"-shm"}, std::string_view{"-journal"}})
            {
                std::filesystem::path sidecar = m_path;
                sidecar += suffix;
                std::filesystem::remove(sidecar, ignored);
            }
        }

        std::filesystem::path m_path; ///< 临时数据库文件绝对路径
    };

    /// @return true 文本里至少有一个非 ASCII 字节，即「驱动自己写了中文说明」
    bool containsChinese(const std::string &text)
    {
        return std::any_of(text.begin(), text.end(),
                           [](const char character)
                           {
                               return static_cast<unsigned char>(character) >= 0x80;
                           });
    }

    /// 取当前行某一列的原始值：列键可以是下标或列名，由 getValue 的两个重载去分
    template<typename ColumnKey>
    [[nodiscard]] Database::DatabaseValue cellAt(const Database::DatabaseResult &result, const ColumnKey &columnKey)
    {
        return result.getValue(columnKey);
    }

    /**
     * @brief 按期望类型安全地取一列
     * @details getValue 交出的是值语义的 variant：拿它的地址去 get_if 会在整条表达式结束时就悬垂，
     *          所以这里先落成局部量再取值
     * @tparam CellType 期望的备选类型
     * @tparam ColumnKey 列键类型（std::size_t 或 std::string_view）
     * @param result 结果集，游标须停在有效行上
     * @param columnKey 列下标或列名
     * @return std::optional<CellType> 列值；类型不符、越界或该列为 NULL 时返回空
     */
    template<typename CellType, typename ColumnKey>
    [[nodiscard]] std::optional<CellType> cellAs(const Database::DatabaseResult &result, const ColumnKey &columnKey)
    {
        const Database::DatabaseValue cellValue = cellAt(result, columnKey);
        const auto                   *typedCell = std::get_if<CellType>(&cellValue);
        return typedCell == nullptr ? std::nullopt : std::optional<CellType>(*typedCell);
    }

    /// 该列是否为 SQL NULL：「没有值」与「值恰好为空」必须在这里就分开
    template<typename ColumnKey>
    [[nodiscard]] bool isNullCell(const Database::DatabaseResult &result, const ColumnKey &columnKey)
    {
        return std::holds_alternative<std::monostate>(cellAt(result, columnKey));
    }

    /// 读一条只返回单个整数的语句（COUNT(*) / PRAGMA）；失败或没有行时返回空
    std::optional<std::int64_t> scalarInteger(Database::DatabaseConnection &connection, const std::string_view sql)
    {
        const std::unique_ptr<Database::DatabaseResult> result = connection.execute(sql);
        if (result == nullptr || !result->next())
        {
            return std::nullopt;
        }
        return cellAs<std::int64_t>(*result, std::size_t{0});
    }

    /// 读一条只返回单个文本的语句（PRAGMA journal_mode / typeof()）；失败或没有行时返回空
    std::optional<std::string> scalarText(Database::DatabaseConnection &connection, const std::string_view sql)
    {
        const std::unique_ptr<Database::DatabaseResult> result = connection.execute(sql);
        if (result == nullptr || !result->next())
        {
            return std::nullopt;
        }
        return cellAs<std::string>(*result, std::size_t{0});
    }

    /// 取环境变量；未设置时回落到默认值（与 tests/Database/DatabaseTestSupport.h 同一口径）
    std::string readEnvironmentText(const std::string_view variableName, const std::string_view fallback)
    {
        const std::optional<std::string> value = Platform::ProcessInfo::environmentVariable(std::string{variableName});
        return (value.has_value() && !value->empty()) ? *value : std::string{fallback};
    }

    /// 取端口型环境变量；非法取值一律回落，不因为环境写错就让整步自检失败
    std::uint16_t readEnvironmentPort(const std::string_view variableName, const std::uint16_t fallback)
    {
        const std::string portText =
                Platform::ProcessInfo::environmentVariable(std::string{variableName}).value_or(std::string{});
        int        parsedPort = 0;
        const auto result     = std::from_chars(portText.data(), portText.data() + portText.size(), parsedPort);
        if (portText.empty() || result.ec != std::errc{} || result.ptr != portText.data() + portText.size() || parsedPort <= 0 ||
            parsedPort > 65535)
        {
            return fallback;
        }
        return static_cast<std::uint16_t>(parsedPort);
    }

    /// 取整数型环境变量（Redis 键空间编号用）；非法取值回落默认值
    int readEnvironmentInteger(const std::string_view variableName, const int fallback)
    {
        const std::string valueText =
                Platform::ProcessInfo::environmentVariable(std::string{variableName}).value_or(std::string{});
        int        parsedValue = 0;
        const auto result      = std::from_chars(valueText.data(), valueText.data() + valueText.size(), parsedValue);
        if (valueText.empty() || result.ec != std::errc{} || result.ptr != valueText.data() + valueText.size())
        {
            return fallback;
        }
        return parsedValue;
    }

    /// 读一条 Redis 命令的标量整数回复；命令失败或回复不是整数时返回空
    std::optional<std::int64_t> readIntegerReply(Database::RedisConnection &connection, const std::vector<std::string_view> &arguments)
    {
        const std::unique_ptr<Database::DatabaseResult> reply = connection.executeCommand(arguments);
        if (reply == nullptr)
        {
            return std::nullopt;
        }
        return cellAs<std::int64_t>(*reply, std::size_t{0});
    }

    /// 数一段文本里某个字符出现了几次
    std::size_t countCharacter(const std::string_view text, const char target)
    {
        return static_cast<std::size_t>(std::ranges::count(text, target));
    }

    /**
     * @brief 造一个 SQLite 连接工厂：池的契约要求交出「已经 connect() 完成」的连接
     * @param databasePath UTF-8 的库路径，":memory:" 得到内存库
     */
    std::function<std::unique_ptr<Database::DatabaseConnection>()> makeSqliteFactory(const std::string &databasePath)
    {
        return [databasePath]() -> std::unique_ptr<Database::DatabaseConnection>
        {
            auto connection =
                    std::make_unique<Database::SqliteConnection>(Database::ConnectionConfig::sqliteDefault(databasePath));
            static_cast<void>(connection->connect());
            return connection;
        };
    }

    /**
     * @brief 跑一步自检：框架异常绝不逃出 main，抛出来就折成一条失败结论
     * @param stepName 步骤名，出现在日志与失败结论里
     * @param body 步骤体
     */
    void runStep(const std::string_view stepName, const std::function<void ()> &body)
    {
        LOG_INFO_FMT("—— 自检步骤：{} ——", stepName);
        try
        {
            body();
        }
        catch (const Database::DatabaseException &caught)
        {
            LOG_ERROR_FMT("数据库异常：{}", caught.what());
            Samples::checklist().check(false, std::string("步骤「") + std::string(stepName) + "」没有数据库异常逃出");
        }
        catch (const Base::Exception &caught)
        {
            LOG_ERROR_FMT("框架异常：{}", caught.what());
            Samples::checklist().check(false, std::string("步骤「") + std::string(stepName) + "」没有框架异常逃出");
        }
        catch (const std::exception &caught)
        {
            LOG_ERROR_FMT("标准异常：{}", caught.what());
            Samples::checklist().check(false, std::string("步骤「") + std::string(stepName) + "」没有标准异常逃出");
        }
    }

    /**
     * @brief 跑一步依赖外部环境变量的真机自检：条件不满足时只记一行跳过原因
     * @param stepName 步骤名
     * @param isAvailable 环境是否齐备（驱动已编译且必需变量已设置）
     * @param skipReason 跳过原因，只写变量名，绝不带上取值
     * @param body 步骤体
     */
    void runGatedStep(const std::string_view stepName, const bool isAvailable, const std::string_view skipReason,
                      const std::function<void ()> &body)
    {
        if (!isAvailable)
        {
            LOG_INFO_FMT("跳过「{}」：{}（这一步不计入通过也不计入失败）", stepName, skipReason);
            return;
        }
        runStep(stepName, body);
    }

    // ========================================================================
    // 1. SQLite 文件库
    // ========================================================================

    void demonstrateSqliteFileDatabase(const TemporaryDatabaseFile &databaseFile)
    {
        const Database::ConnectionConfig configuration = Database::ConnectionConfig::sqliteDefault(databaseFile.utf8Path());
        Database::SqliteConnection       connection(configuration);
        connection.setQueryTimeout(4321);

        Samples::checklist().check(!databaseFile.exists() && connection.connect() && connection.isConnected() && databaseFile.exists(),
                                   "SQLite 文件库：connect() 之前文件不存在，之后被引擎创建出来");

        // busy_timeout 由引擎回答，比信返回值更硬：基类的 queryTimeout() 确实映射进来了
        Samples::checklist().check(scalarInteger(connection, "PRAGMA busy_timeout") == std::optional<std::int64_t>(4321),
                                   "queryTimeout() 真的映射成了 PRAGMA busy_timeout");

        const std::unique_ptr<Database::DatabaseResult> createResult = connection.execute(kCreateRawTableSql);
        bool                                            isSeeded     = createResult != nullptr;
        if (isSeeded)
        {
            isSeeded = connection.execute("INSERT INTO raw_values (id, label, ratio, age, payload)"
                                          " VALUES (1, '张三', 95.5, 30, x'5c0041')") != nullptr;
        }
        if (isSeeded)
        {
            isSeeded = connection.execute("INSERT INTO raw_values (id, label, ratio) VALUES (2, 'Bob', -0.25)") != nullptr;
        }
        const std::optional<std::int64_t> seededRowCount = scalarInteger(connection, "SELECT COUNT(*) FROM raw_values");
        Samples::checklist().check(isSeeded && seededRowCount == std::optional<std::int64_t>(2), "文件库上建表并写入两行原始数据");

        // 游标、列元数据与四种存储类的映射一次验完：只读语句会预扫描，rowCount() 是精确值
        bool isCursorCorrect = false;
        if (const std::unique_ptr<Database::DatabaseResult> selection =
                    connection.execute("SELECT id, label, ratio, age, payload FROM raw_values ORDER BY id"))
        {
            Database::DatabaseResult &result = *selection;
            std::vector<std::int64_t>  ids;
            std::optional<std::string> firstLabel;
            std::optional<double>      firstRatio;
            std::optional<double>      secondRatio;
            bool                       isBlobKeepsEmbeddedNull = false;
            bool                       isAgeMissingOnSecondRow = false;

            for (std::size_t rowIndex = 0; result.next(); ++rowIndex)
            {
                if (const std::optional<std::int64_t> identifier = cellAs<std::int64_t>(result, std::size_t{0}); identifier.has_value())
                {
                    ids.push_back(identifier.value());
                }

                if (rowIndex == 0)
                {
                    firstLabel = cellAs<std::string>(result, std::string_view{"label"});
                    firstRatio = cellAs<double>(result, std::size_t{2});
                    // x'5c0041' 是 0x5C、0x00、0x41 三个字节：按长度拷贝才保得住中间那个 '\0'
                    if (const std::optional<Database::BinaryBytes> bytes = cellAs<Database::BinaryBytes>(result, std::string_view{"payload"});
                        bytes.has_value())
                    {
                        isBlobKeepsEmbeddedNull =
                                bytes->size() == 3 && (*bytes)[0] == 0x5C && (*bytes)[1] == 0x00 && (*bytes)[2] == 0x41;
                    }
                }
                else
                {
                    secondRatio           = cellAs<double>(result, std::string_view{"ratio"});
                    isAgeMissingOnSecondRow = isNullCell(result, std::string_view{"age"});
                }
            }

            // reset() 之后能从头再走一遍，且第二次的第一行仍是 id=1
            result.reset();
            const bool isRewound = result.next() && cellAs<std::int64_t>(result, std::size_t{0}) == std::optional<std::int64_t>(1);

            isCursorCorrect = ids == std::vector<std::int64_t>{1, 2} && firstLabel == std::optional<std::string>("张三") &&
                              firstRatio == std::optional<double>(95.5) && secondRatio == std::optional<double>(-0.25) &&
                              isAgeMissingOnSecondRow && isBlobKeepsEmbeddedNull && isRewound && result.rowCount() == 2 &&
                              result.columnCount() == 5 && !result.isEmpty() && result.columnName(1) == std::optional<std::string>("label") &&
                              result.columnIndex("ratio") == std::optional<std::size_t>(2);
        }
        Samples::checklist().check(
                isCursorCorrect,
                "结果集游标逐列读回：整数/文本/浮点/NULL 各归其类，内嵌 '\\0' 的二进制按长度保真，reset() 能重新遍历");

        // 写回执上的两个计数：影响行数走基类虚接口即可读到，不必向下转型
        bool isReceiptCorrect = false;
        if (const std::unique_ptr<Database::DatabaseResult> receipt =
                    connection.execute("INSERT INTO raw_values (id, label) VALUES (500, 'Explicit')"))
        {
            isReceiptCorrect = receipt->affectedRowCount() == 1 && connection.lastInsertRowId() == 500;
            if (auto *sqliteReceipt = dynamic_cast<Database::SqliteResult *>(receipt.get()); sqliteReceipt != nullptr)
            {
                isReceiptCorrect = isReceiptCorrect && sqliteReceipt->lastInsertRowId() == 500;
            }
            else
            {
                LOG_WARN("驱动交出的结果集不是 SqliteResult：派生类型上的 lastInsertRowId() 无从验证");
                isReceiptCorrect = false;
            }
        }
        Samples::checklist().check(isReceiptCorrect,
                                   "写回执：affectedRowCount() 经基类接口读到 1，rowid 计数器被显式主键刷新到 500");

        // 落盘才算「文件库」：断开后文件有内容，换第二个连接对象仍能读到已提交的行
        connection.disconnect();
        Samples::checklist().check(!connection.isConnected() && connection.nativeHandle() == nullptr && databaseFile.fileSizeBytes() > 0,
                                   "disconnect() 释放句柄，磁盘上的库文件仍留有内容");

        Database::SqliteConnection reader(configuration);
        const bool                 isReaderConnected = reader.connect();
        Samples::checklist().check(
                isReaderConnected && scalarInteger(reader, "SELECT COUNT(*) FROM raw_values") == std::optional<std::int64_t>(3) &&
                        scalarText(reader, "PRAGMA journal_mode") == std::optional<std::string>("wal"),
                "第二个连接对象读到已提交的三行，且文件库确实跑在 WAL 模式下");
        Samples::checklist().check(reader.serverVersion().find('.') != std::string::npos && !reader.serverVersion().empty(),
                                   "serverVersion() 给出链接进来的 SQLite 版本号");
    }

    // ========================================================================
    // 2. SQLite 内存库
    // ========================================================================

    void demonstrateSqliteInMemoryDatabase()
    {
        // 头文件约定的内存库写法：sqliteDefault() 的默认路径就是 ":memory:"
        Database::SqliteConnection connection(Database::ConnectionConfig::sqliteDefault());
        Samples::checklist().check(connection.connect() && connection.nativeHandle() != nullptr && connection.lastError().empty(),
                                   "SQLite 内存库按 \":memory:\" 连上，启动期 PRAGMA 不残留错误文本");

        const bool isSeeded = connection.execute(kCreateRawTableSql) != nullptr &&
                              connection.execute("INSERT INTO raw_values (id, label, ratio, age) VALUES (1, '甲', 0.5, 7)") != nullptr;
        Samples::checklist().check(
                isSeeded && scalarInteger(connection, "SELECT COUNT(*) FROM raw_values") == std::optional<std::int64_t>(1) &&
                        scalarText(connection, "PRAGMA journal_mode") == std::optional<std::string>("memory"),
                "内存库上同样跑得通建表 + 写入 + 读回，日志模式如实报 memory");

        // 空串路径按内存库兜底（构造函数注释的承诺），journal_mode 是唯一可观察的证据
        Database::ConnectionConfig emptyConfiguration;
        emptyConfiguration.database = "";
        Database::SqliteConnection fallbackConnection(emptyConfiguration);
        Samples::checklist().check(fallbackConnection.connect() &&
                                           scalarText(fallbackConnection, "PRAGMA journal_mode") == std::optional<std::string>("memory"),
                                   "database 为空串时兜底成内存库并立即可用");

        // 内存库随句柄销毁一起清空：重连拿到的是全新空库，失败原因里既有中文也有引擎原文
        connection.disconnect();
        Samples::checklist().check(connection.connect() && connection.execute("SELECT * FROM raw_values") == nullptr &&
                                           containsChinese(connection.lastError()) &&
                                           connection.lastError().find("no such table") != std::string::npos,
                                   "断开重开后内存库被清空，缺表失败给出「中文说明 + 引擎原文 + 错误码」");
    }

    // ========================================================================
    // 3. 连接工厂与类型枚举
    // ========================================================================

    void demonstrateFactoryAndTypes(const TemporaryDatabaseFile &databaseFile)
    {
        // 工厂只挑驱动、不做连接：交出的基类指针仍要调用方自己 connect()
        std::unique_ptr<Database::DatabaseConnection> connection =
                Database::DatabaseFactory::createSqlite(Database::ConnectionConfig::sqliteDefault(databaseFile.utf8Path()));
        Samples::checklist().check(
                connection != nullptr && connection->databaseType() == Database::DatabaseType::Sqlite &&
                        std::string_view(Database::databaseTypeName(Database::DatabaseType::Sqlite)) == "Sqlite" &&
                        !connection->isConnected() && connection->connect() && databaseFile.exists() &&
                        connection->configuration().database == databaseFile.utf8Path(),
                "工厂交出基类指针：不替调用方建连，connect() 后文件落盘且配置原样可读回");

        static_cast<void>(connection->execute(kCreateRawTableSql));
        static_cast<void>(connection->execute("INSERT INTO raw_values (id, label) VALUES (1, '工厂写入')"));
        const std::unique_ptr<Database::DatabaseResult> selection = connection->execute("SELECT label FROM raw_values");
        Samples::checklist().check(scalarInteger(*connection, "SELECT COUNT(*) FROM raw_values") == std::optional<std::int64_t>(1) &&
                                           selection != nullptr && selection->next() &&
                                           cellAs<std::string>(*selection, std::size_t{0}) == std::optional<std::string>("工厂写入"),
                                   "经基类指针读写文件库：虚派发把 SQL 真的送到了 SQLite 驱动");

        Samples::checklist().check(
                Database::DatabaseFactory::guessType(3306) == std::optional<Database::DatabaseType>(Database::DatabaseType::MySql) &&
                        Database::DatabaseFactory::guessType(6379) == std::optional<Database::DatabaseType>(Database::DatabaseType::Redis) &&
                        !Database::DatabaseFactory::guessType(5432).has_value(),
                "guessType 只认已实现引擎的默认端口，未知端口给空而不是回退成 MySQL");

        Database::ConnectionConfig redisStyleConfiguration;
        redisStyleConfiguration.host     = "127.0.0.1";
        redisStyleConfiguration.port     = 6379;
        redisStyleConfiguration.database = "15";
        const std::unique_ptr<Database::DatabaseConnection> guessedConnection = Database::DatabaseFactory::create(redisStyleConfiguration);
        Samples::checklist().check(guessedConnection != nullptr && guessedConnection->databaseType() == Database::DatabaseType::Redis,
                                   "按配置推断类型：6379 的配置造出 Redis 驱动");

        // 父目录不存在且刻意不创建：这是无需权限即可稳定复现的打开失败路径
        const std::filesystem::path missingDirectoryPath = std::filesystem::temp_directory_path() /
                                                           "asyn-sample-database-missing-directory" /
                                                           std::to_string(Platform::ProcessInfo::currentProcessId()) / "database.db";
        Database::ConnectionConfig brokenConfiguration;
        brokenConfiguration.database = toUtf8PathText(missingDirectoryPath);
        Database::SqliteConnection brokenConnection(brokenConfiguration);
        Samples::checklist().check(!brokenConnection.connect() && !brokenConnection.isConnected() &&
                                           brokenConnection.nativeHandle() == nullptr &&
                                           brokenConnection.lastError().find("打开") != std::string::npos &&
                                           brokenConnection.lastError().find(brokenConfiguration.database) != std::string::npos,
                                   "打不开文件时 connect() 如实失败：不留半开句柄，中文原因里带着请求的路径");
    }

    // ========================================================================
    // 4. SQL 方言层
    // ========================================================================

    void demonstrateDialectLayer()
    {
        const Database::SqliteDialect sqliteDialect;
        const Database::MySqlDialect  mySqlDialect;

        Samples::checklist().check(sqliteDialect.quoteIdentifier(R"(a"b)") == R"("a""b")" && mySqlDialect.quoteIdentifier("a`b") == "`a``b`",
                                   "标识符引用：双引号与反引号各自的引用字符都靠翻倍转义");

        Database::Queryable::QueryNode queryNode;
        queryNode.tableName       = std::string(kAccountTableName);
        queryNode.selectColumns   = {"id", "name"};
        queryNode.whereConditions.push_back(kAccountIdColumn >= std::int64_t{2});
        queryNode.whereConditions.push_back(Database::Queryable::in(kAccountIdColumn, std::vector<std::int64_t>{2, 3}));
        queryNode.orderBy.push_back(Database::Queryable::desc("id"));
        queryNode.limit = 5;

        const Database::SqlStatement statement = sqliteDialect.translate(queryNode);
        Samples::checklist().check(
                statement.sql.find("WHERE") != std::string::npos && !statement.sql.empty() &&
                        countCharacter(statement.sql, '?') == statement.parameters.size() && statement.parameters.size() == 3,
                "方言把查询树翻成参数化 SQL：占位符个数与按序收集绑定参数个数严格相等（IN 展开成逐个值）");
        // 引用必须落在标识符上：漏引号时 SQLite 会把带空格的名字当语法片段，整条语句作废
        Samples::checklist().check(statement.sql.find("\"sample accounts\"") != std::string::npos &&
                                           statement.sql.find("\"name\"") != std::string::npos && statement.sql.find("LIMIT 5") != std::string::npos,
                                   "含空格的表名与列名在翻译后的 SQL 里被整段引用，分页按方言补全");

        Samples::checklist().check(
                sqliteDialect.beginTransactionStatement() == "BEGIN IMMEDIATE" &&
                        mySqlDialect.beginTransactionStatement() == "START TRANSACTION" && sqliteDialect.commitStatement() == "COMMIT" &&
                        sqliteDialect.rollbackStatement() == "ROLLBACK",
                "事务控制语句的文本由方言给出，SQLite 与 MySQL 的写法在此处分家");

        // 两个引擎的能力差异：参数上限与「能不能直接做主键」的物理类型
        Samples::checklist().check(
                sqliteDialect.maximumStatementParameters() == 999 && mySqlDialect.maximumStatementParameters() > 999 &&
                        sqliteDialect.columnTypeName(Database::ColumnType::Blob) == "BLOB" &&
                        mySqlDialect.columnTypeName(Database::ColumnType::Blob) == "LONGBLOB" &&
                        mySqlDialect.keyColumnTypeName(Database::ColumnType::Blob) != mySqlDialect.columnTypeName(Database::ColumnType::Blob),
                "引擎知识落在方言层：参数上限、二进制类型名与「可作主键的类型」三项都按引擎区分");

        Samples::checklist().check(
                Database::DialectRegistry::supports(Database::DatabaseType::Sqlite) &&
                        Database::DialectRegistry::supports(Database::DatabaseType::MySql) &&
                        !Database::DialectRegistry::supports(Database::DatabaseType::Redis) &&
                        Database::DialectRegistry::dialectFor(Database::DatabaseType::Sqlite) ==
                                Database::DialectRegistry::dialectFor(Database::DatabaseType::Sqlite),
                "方言注册表：SQL 引擎有方言且给出同一个无状态实例，Redis 被判定为不支持");

        bool isRedisRejectedWithChineseReason = false;
        try
        {
            static_cast<void>(Database::DialectRegistry::dialectFor(Database::DatabaseType::Redis));
        }
        catch (const std::exception &caught)
        {
            isRedisRejectedWithChineseReason = containsChinese(caught.what());
        }
        Samples::checklist().check(isRedisRejectedWithChineseReason, "向注册表要 Redis 方言时抛出中文提示而不是返回空指针");
    }

    // ========================================================================
    // 5. ORM 与查询构建
    // ========================================================================

    /**
     * @brief 注入样本行：户名里同时有单引号、"--" 与分号，只有按绑定方式送入才可能原样存回
     * @return AccountRow 主键 2 的那一行
     */
    [[nodiscard]] AccountRow makeHostileSampleRow()
    {
        return AccountRow{.id = 2, .name = "O'Brien -- DROP TABLE sample accounts; --", .balance = -99.5, .note = std::nullopt,
                          .active = false};
    }

    void demonstrateObjectRelationalMapping(Database::ConnectionPool &pool)
    {
        // 表名含空格：DDL 与 DML 的引用规则必须一致，否则建出来的表根本查不动
        const Database::SqlStatement createStatement =
                Database::Queryable::SchemaMigrator::createTableStatement<AccountRow>(*Database::DialectRegistry::dialectFor(
                        Database::DatabaseType::Sqlite));
        Samples::checklist().check(createStatement.sql.find("CREATE TABLE IF NOT EXISTS \"sample accounts\"") == 0 &&
                                           createStatement.parameters.empty() &&
                                           createStatement.sql.find("\"note\" TEXT,") != std::string::npos &&
                                           createStatement.sql.find("\"active\" INTEGER NOT NULL") != std::string::npos,
                                   "建表语句由 TableSchema 生成：可空列不加 NOT NULL、参数列表恒为空");

        std::string errorText;
        const bool  isCreated = Database::Queryable::SchemaMigrator::createTable<AccountRow>(pool, true, &errorText);
        Samples::checklist().check(isCreated && errorText.empty() &&
                                           Database::Queryable::SchemaMigrator::tableExists<AccountRow>(pool, &errorText) && errorText.empty() &&
                                           !Database::Queryable::SchemaMigrator::tableExists<DocumentRow>(pool, &errorText) &&
                                           errorText.empty(),
                                   "SchemaMigrator 建表成功，tableExists 能区分「已建的表」与「还没建的表」");

        OrmQuery<AccountRow> insertQuery(pool, Database::DatabaseType::Sqlite);
        const std::int64_t   firstInserted = insertQuery.insert(
                AccountRow{.id = 1, .name = "张三", .balance = 1234.56, .note = std::string("普通备注"), .active = true});
        const std::int64_t secondInserted = insertQuery.insert(makeHostileSampleRow());
        const std::int64_t thirdInserted  = insertQuery.insert(
                AccountRow{.id = 3, .name = "李四", .balance = 0.0, .note = std::string("中文备注"), .active = true});

        OrmQuery<AccountRow> countQuery(pool, Database::DatabaseType::Sqlite);
        Samples::checklist().check(firstInserted == 1 && secondInserted == 1 && thirdInserted == 1 && countQuery.count() == 3,
                                   "ORM 插入三行各回报 1 行受影响，count() 数到三行");

        OrmQuery<AccountRow> allQuery(pool, Database::DatabaseType::Sqlite);
        const std::vector<AccountRow> rows = allQuery.orderBy(Database::Queryable::asc("id")).toList();
        // 第二行的户名是注入样本：原样读回 + 表还在，说明取值确实以绑定方式送入而不是拼进文本
        Samples::checklist().check(
                rows.size() == 3 && rows[0].name == "张三" && rows[0].balance == 1234.56 &&
                        rows[0].note == std::optional<std::string>("普通备注") && rows[0].active && rows[1].id == 2 &&
                        rows[1].name == makeHostileSampleRow().name && !rows[1].note.has_value() && rows[1].balance == -99.5 &&
                        !rows[1].active && rows[2].name == "李四" && rows[2].balance == 0.0 &&
                        OrmQuery<AccountRow>(pool, Database::DatabaseType::Sqlite).count() == 3,
                "按主键升序读回三行：中文、含单引号与 \"--\" 的注入文本、负浮点、NULL→空 optional、0/1→bool 全部映射正确");

        OrmQuery<AccountRow> orderedQuery(pool, Database::DatabaseType::Sqlite);
        const std::vector<AccountRow> topRows =
                orderedQuery.where(kAccountIdColumn >= std::int64_t{1}).orderBy(Database::Queryable::desc("id")).limit(1).toList();
        Samples::checklist().check(topRows.size() == 1 && topRows[0].id == 3 && topRows[0].note == std::optional<std::string>("中文备注"),
                                   "WHERE + 降序 + LIMIT 交给数据库做：只回一行且是 id=3");

        OrmQuery<AccountRow> filteredQuery(pool, Database::DatabaseType::Sqlite);
        const std::vector<AccountRow> matched = filteredQuery
                                                        .where(Database::Queryable::in(kAccountIdColumn, std::vector<std::int64_t>{1, 3}) &&
                                                               Database::Queryable::like(kAccountNameColumn, std::string("%张%")))
                                                        .toList();
        Samples::checklist().check(matched.size() == 1 && matched[0].id == 1, "IN 与 LIKE 复合条件递归渲染，只命中 id=1 那一行");

        OrmQuery<AccountRow> missingQuery(pool, Database::DatabaseType::Sqlite);
        Samples::checklist().check(!missingQuery.where(kAccountIdColumn == std::int64_t{999}).first().has_value(),
                                   "first() 在无匹配行时给出空 optional 而不是抛异常");

        OrmQuery<AccountRow> activeCountQuery(pool, Database::DatabaseType::Sqlite);
        OrmQuery<AccountRow> negativeCountQuery(pool, Database::DatabaseType::Sqlite);
        Samples::checklist().check(activeCountQuery.where(kAccountActiveColumn == true).count() == 2 &&
                                           negativeCountQuery.where(kAccountBalanceColumn < 0.0).count() == 1,
                                   "带条件的 count() 按谓词过滤：启用两行、负余额一行");

        OrmQuery<AccountRow> updateQuery(pool, Database::DatabaseType::Sqlite);
        const std::int64_t   updatedRows =
                updateQuery.update(AccountRow{.id = 2, .name = "王五", .balance = 888.25, .note = std::string("已更新"), .active = true});
        OrmQuery<AccountRow> verifyQuery(pool, Database::DatabaseType::Sqlite);
        const std::vector<AccountRow> afterUpdate = verifyQuery.orderBy(Database::Queryable::asc("id")).toList();
        Samples::checklist().check(updatedRows == 1 && afterUpdate.size() == 3 && afterUpdate[1].name == "王五" &&
                                           afterUpdate[1].balance == 888.25 && afterUpdate[1].note == std::optional<std::string>("已更新") &&
                                           afterUpdate[0].name == "张三" && afterUpdate[2].name == "李四",
                                   "按主键更新只改目标行：主键不进 SET，其它两行原样保留");

        OrmQuery<AccountRow> pagedQuery(pool, Database::DatabaseType::Sqlite);
        const std::vector<AccountRow> paged = pagedQuery.orderBy(Database::Queryable::asc("id")).limit(2).offset(1).toList();
        Samples::checklist().check(paged.size() == 2 && paged[0].id == 2 && paged[1].id == 3, "LIMIT + OFFSET 分页取到中间两行");

        // AccountRow 有 5 列、SQLite 单条语句上限 999 个参数 → 每批 199 行；300 行必然被拆成两块
        std::vector<AccountRow> batchRows;
        batchRows.reserve(300);
        for (std::int64_t id = 100; id < 400; ++id)
        {
            batchRows.push_back(AccountRow{.id = id, .name = "批量-" + std::to_string(id), .balance = static_cast<double>(id), .note = std::nullopt,
                                           .active = false});
        }
        OrmQuery<AccountRow> batchQuery(pool, Database::DatabaseType::Sqlite);
        const std::int64_t   batchInsertedRows = batchQuery.insertBatch(batchRows);

        OrmQuery<AccountRow> totalQuery(pool, Database::DatabaseType::Sqlite);
        const std::int64_t totalRowCount = totalQuery.count();

        OrmQuery<AccountRow> spotQuery(pool, Database::DatabaseType::Sqlite);
        const std::optional<AccountRow> secondChunkRow = spotQuery.where(kAccountIdColumn == std::int64_t{250}).first();
        Samples::checklist().check(batchInsertedRows == 300 && totalRowCount == 303 && secondChunkRow.has_value() &&
                                           secondChunkRow->name == "批量-250" && secondChunkRow->balance == 250.0 && !secondChunkRow->note.has_value(),
                                   "批量插入超过参数上限时自动分块（本地事务覆盖全部块），跨块的行逐列落在正确位置");

        OrmQuery<AccountRow> deleteQuery(pool, Database::DatabaseType::Sqlite);
        const std::int64_t   deletedRows = deleteQuery.where(kAccountIdColumn >= std::int64_t{100}).executeNonQuery();
        OrmQuery<AccountRow> remainingQuery(pool, Database::DatabaseType::Sqlite);
        Samples::checklist().check(deletedRows == 300 && remainingQuery.count() == 3, "executeNonQuery 按条件删除并回报行数");

        OrmQuery<AccountRow> clearQuery(pool, Database::DatabaseType::Sqlite);
        OrmQuery<AccountRow> clearedCountQuery(pool, Database::DatabaseType::Sqlite);
        Samples::checklist().check(clearQuery.executeNonQuery() == 3 && clearedCountQuery.count() == 0,
                                   "不带条件的 executeNonQuery 清空全表（文档承诺的语义，写侧同样走方言）");

        OrmQuery<AccountRow> offlineQuery;
        bool                 isOfflineExecutionRejected = false;
        std::string          offlineReason;
        try
        {
            static_cast<void>(offlineQuery.toList());
        }
        catch (const std::exception &caught)
        {
            isOfflineExecutionRejected = true;
            offlineReason              = caught.what();
        }
        Samples::checklist().check(offlineQuery.toSql() == "SELECT * FROM sample accounts" && isOfflineExecutionRejected &&
                                           containsChinese(offlineReason),
                                   "离线模式只做 SQL 生成（toSql 给近似文本），拿它执行会抛出中文逻辑错误");
    }

    // ========================================================================
    // 6. 事务
    // ========================================================================

    void demonstrateTransactions(Database::ConnectionPool &pool)
    {
        const auto bystanderCount = [&pool]()
        {
            OrmQuery<AccountRow> query(pool, Database::DatabaseType::Sqlite);
            return query.count();
        };

        {
            Database::Transaction transaction(pool);
            OrmQuery<AccountRow>  transactionalQuery(transaction);
            const std::int64_t firstInserted  = transactionalQuery.insert(
                    AccountRow{.id = 1, .name = "提交行", .balance = 1234.56, .note = std::string("普通备注"), .active = true});
            const std::int64_t secondInserted = transactionalQuery.insert(
                    AccountRow{.id = 2, .name = "李四", .balance = -99.5, .note = std::nullopt, .active = false});

            const std::int64_t invisibleRowCount = bystanderCount();
            const bool         isCommitted       = transaction.commit();

            Samples::checklist().check(transaction.isActive() == false && firstInserted == 1 && secondInserted == 1 &&
                                               invisibleRowCount == 0 && isCommitted,
                                       "提交前旁观连接一行都看不到，commit() 才让它生效");

            OrmQuery<AccountRow> readBack(pool, Database::DatabaseType::Sqlite);
            const std::optional<AccountRow> committedRow =
                    readBack.where(kAccountIdColumn == std::int64_t{1}).first();
            Samples::checklist().check(bystanderCount() == 2 && committedRow.has_value() && committedRow->name == "提交行" &&
                                               committedRow->balance == 1234.56 &&
                                               committedRow->note == std::optional<std::string>("普通备注"),
                                       "提交后数据对别的连接可见，字段逐列回读一致");

            // 已结束的事务再喊提交/回滚都不该发出多余语句，更不该撤掉已落库的数据
            Samples::checklist().check(transaction.commit() && transaction.rollback() && bystanderCount() == 2,
                                       "commit 之后再 commit / rollback 都幂等，已提交的数据不受影响");
        }

        {
            Database::Transaction transaction(pool);
            OrmQuery<AccountRow>  transactionalQuery(transaction);
            static_cast<void>(transactionalQuery.insert(
                    AccountRow{.id = 3, .name = "会被回滚", .balance = 1.0, .note = std::nullopt, .active = true}));
            static_cast<void>(transactionalQuery.insert(
                    AccountRow{.id = 4, .name = "也会被回滚", .balance = 2.0, .note = std::nullopt, .active = true}));
            const bool isRolledBack = transaction.rollback();

            Samples::checklist().check(isRolledBack && !transaction.isActive() && bystanderCount() == 2,
                                       "显式回滚撤销本次两行写入，表回到事务开始前的样子");
        }

        {
            Database::Transaction transaction(pool);
            OrmQuery<AccountRow>  transactionalQuery(transaction);
            static_cast<void>(transactionalQuery.insert(
                    AccountRow{.id = 5, .name = "未提交就离开作用域", .balance = 3.0, .note = std::nullopt, .active = true}));
            // 刻意不调用 commit / rollback：析构必须补上回滚，否则未结束的事务会串给下一个借用者
        }
        Samples::checklist().check(bystanderCount() == 2, "事务对象析构时自动回滚，库里仍只有先前提交的两行");

        // 约束违例穿过事务作用域：异常抛出后仍要看到完整回滚，而不是「前一条留下、后一条失败」的半成品
        std::string escapedReason;
        try
        {
            Database::Transaction transaction(pool);
            OrmQuery<AccountRow>  transactionalQuery(transaction);
            static_cast<void>(transactionalQuery.insert(
                    AccountRow{.id = 6, .name = "回滚掉的新行", .balance = 7.0, .note = std::nullopt, .active = true}));
            // 主键 1 已提交：这一条必败，并把整笔事务带进异常路径
            static_cast<void>(transactionalQuery.insert(
                    AccountRow{.id = 1, .name = "重复主键", .balance = 8.0, .note = std::nullopt, .active = true}));
        }
        catch (const std::exception &caught)
        {
            escapedReason = caught.what();
        }
        OrmQuery<AccountRow> survivingQuery(pool, Database::DatabaseType::Sqlite);
        Samples::checklist().check(survivingQuery.count() == 2 && !escapedReason.empty() && containsChinese(escapedReason),
                                   "唯一约束违例抛出的异常里带着中文原因，且那一笔新行随事务一起被回滚");

        // 连接对象上的事务入口（不经池）：语句文本取自方言的 BEGIN IMMEDIATE，嵌套 BEGIN 被拒但不破坏已有事务
        Database::SqliteConnection connection(Database::ConnectionConfig::sqliteDefault());
        static_cast<void>(connection.connect());
        static_cast<void>(connection.execute(kCreateRawTableSql));
        const bool isNestedBeginRejected = connection.beginTransaction() &&
                                           connection.execute("INSERT INTO raw_values (id, label) VALUES (1, 'InsideTx')") != nullptr &&
                                           !connection.beginTransaction() && connection.lastError().find("transaction") != std::string::npos;
        const bool isCommittedByConnection = connection.commit();
        Samples::checklist().check(isNestedBeginRejected && isCommittedByConnection &&
                                           scalarInteger(connection, "SELECT COUNT(*) FROM raw_values") == std::optional<std::int64_t>(1),
                                   "连接级事务入口：BEGIN IMMEDIATE 生效、嵌套 BEGIN 被拒且不破坏已有事务，COMMIT 后写入仍在");

        Samples::checklist().check(!connection.commit() && !connection.rollback() && containsChinese(connection.lastError()) &&
                                           connection.execute("SELECT 1") != nullptr,
                                   "自动提交模式下没有事务可收尾，两个入口都如实失败而连接照常可用");
    }

    // ========================================================================
    // 7. 二进制列逐字节往返
    // ========================================================================

    void demonstrateBinaryColumn(Database::ConnectionPool &pool)
    {
        std::string errorText;
        const bool  isCreated = Database::Queryable::SchemaMigrator::createTable<DocumentRow>(pool, true, &errorText);
        Samples::checklist().check(isCreated, "二进制表建表：" + (errorText.empty() ? std::string("成功") : errorText));

        // 载荷里同时有内嵌 '\0' 与单字节上界 0xFF：这两种形态最容易在「按文本搬运」时被截断或被改写
        const Database::BinaryBytes  payload{0x5C, 0x00, 0x41, 0xFF, 0x00};
        const std::vector<std::byte> rawPayload{std::byte{0x01}, std::byte{0x80}, std::byte{0x00}};

        OrmQuery<DocumentRow> insertQuery(pool, Database::DatabaseType::Sqlite);
        const bool isInserted = insertQuery.insert(DocumentRow{.id = 1, .payload = payload, .rawPayload = rawPayload, .note = Database::BinaryBytes{}}) == 1 &&
                                insertQuery.insert(DocumentRow{.id = 2, .payload = {}, .rawPayload = {}, .note = std::nullopt}) == 1;
        Samples::checklist().check(isInserted, "两种二进制成员拼法都能按 ORM 插入，零长载荷写进 NOT NULL 列也没被当成 NULL");

        // 列声明成 BLOB 只决定「怎么声明」，值是否真按 BLOB 存取决于绑定方式：问引擎的 typeof()
        std::optional<std::string> payloadStorageClass;
        std::optional<std::string> rawStorageClass;
        {
            Database::PooledConnection connection = pool.acquire();
            if (static_cast<bool>(connection))
            {
                if (const std::unique_ptr<Database::DatabaseResult> result =
                            connection->execute("SELECT typeof(payload), typeof(raw_payload) FROM \"sample documents\" WHERE id = 2"))
                {
                    if (result->next())
                    {
                        payloadStorageClass = cellAs<std::string>(*result, std::size_t{0});
                        rawStorageClass     = cellAs<std::string>(*result, std::size_t{1});
                    }
                }
            }
        }
        Samples::checklist().check(payloadStorageClass == std::optional<std::string>("blob") &&
                                           rawStorageClass == std::optional<std::string>("blob"),
                                   "typeof() 证明两列都按真正的 BLOB 存储类落库，而不是被按文本绑定");

        OrmQuery<DocumentRow> query(pool, Database::DatabaseType::Sqlite);
        const std::vector<DocumentRow> rows = query.orderBy(Database::Queryable::asc("id")).toList();
        Samples::checklist().check(rows.size() == 2 && rows[0].payload == payload && rows[0].rawPayload == rawPayload,
                                   "二进制列逐字节往返：内嵌 '\\0' 与 0xFF 都不丢，两种拼法互逆");

        // 空 BLOB 与 SQL NULL 是两件事：前者「有值且长度为 0」，后者「没有值」
        Samples::checklist().check(rows.size() == 2 && rows[0].note.has_value() && rows[0].note->empty() && !rows[1].note.has_value() &&
                                           rows[1].payload.empty(),
                                   "零长 BLOB 与 NULL 在往返后仍保持区分");

        OrmQuery<DocumentRow> byPayload(pool, Database::DatabaseType::Sqlite);
        const std::vector<DocumentRow> matched = byPayload.where(kDocumentPayloadColumn == payload).toList();
        Samples::checklist().check(matched.size() == 1 && matched[0].id == 1, "按二进制列做等值条件也能命中（参数走二进制备选而不是文本）");
    }

    // ========================================================================
    // 8. 参数绑定与拒绝路径
    // ========================================================================

    void demonstrateParameterBindingAndRejections(Database::ConnectionPool &pool)
    {
        Database::SqliteConnection connection(Database::ConnectionConfig::sqliteDefault(":memory:"));
        static_cast<void>(connection.connect());
        static_cast<void>(connection.execute(kCreateRawTableSql));

        const std::vector<Database::DatabaseValue> scalarParameters{
                std::int64_t{7}, std::string("O'Brien -- 中文"), -12.5, true, Database::BinaryBytes{0x00, 0xFF, 0x10}};
        const std::unique_ptr<Database::DatabaseResult> insertReceipt =
                connection.execute("INSERT INTO raw_values (id, label, ratio, age, payload) VALUES (?, ?, ?, ?, ?)", scalarParameters);

        bool isRoundTripCorrect = insertReceipt != nullptr;
        if (isRoundTripCorrect)
        {
            const std::vector<Database::DatabaseValue> selectParameters{std::int64_t{7}};
            if (const std::unique_ptr<Database::DatabaseResult> selection =
                        connection.execute("SELECT label, ratio, age, payload, typeof(payload) AS payloadType FROM raw_values WHERE id = ?",
                                           selectParameters))
            {
                isRoundTripCorrect = selection->next();
                if (isRoundTripCorrect)
                {
                    const std::optional<Database::BinaryBytes> payloadBytes = cellAs<Database::BinaryBytes>(*selection, std::string_view{"payload"});
                    isRoundTripCorrect =
                            cellAs<std::string>(*selection, std::string_view{"label"}) == std::optional<std::string>("O'Brien -- 中文") &&
                            cellAs<double>(*selection, std::string_view{"ratio"}) == std::optional<double>(-12.5) &&
                            cellAs<std::int64_t>(*selection, std::string_view{"age"}) == std::optional<std::int64_t>(1) &&
                            cellAs<std::string>(*selection, std::string_view{"payloadType"}) == std::optional<std::string>("blob") &&
                            payloadBytes.has_value() && payloadBytes->size() == 3 && (*payloadBytes)[0] == 0x00 && (*payloadBytes)[1] == 0xFF;
                }
            }
            else
            {
                isRoundTripCorrect = false;
            }
        }
        Samples::checklist().check(isRoundTripCorrect,
                                   "五类标量与二进制参数按位置绑定：含单引号与 \"--\" 的文本原样存回，二进制仍落在 blob 存储类");

        // monostate 绑成真 NULL；空串绑上去 IS NULL 不成立——两者绝不能被混为一谈。
        // label 带 NOT NULL 约束，因此 NULL 探针落在可空的 ratio 列上（拿 label 试 NULL 会先撞上约束）
        const std::vector<Database::DatabaseValue> nullParameters{std::int64_t{8}, std::string("not-null"), std::monostate{}};
        const std::vector<Database::DatabaseValue> emptyParameters{std::int64_t{9}, std::string(""), std::int64_t{0}};
        const bool isNullAndEmptyDistinct =
                connection.execute("INSERT INTO raw_values (id, label, ratio) VALUES (?, ?, ?)", nullParameters) != nullptr &&
                connection.execute("INSERT INTO raw_values (id, label, ratio) VALUES (?, ?, ?)", emptyParameters) != nullptr &&
                scalarInteger(connection, "SELECT ratio IS NULL FROM raw_values WHERE id = 8") == std::optional<std::int64_t>(1) &&
                scalarInteger(connection, "SELECT ratio IS NULL FROM raw_values WHERE id = 9") == std::optional<std::int64_t>(0) &&
                scalarInteger(connection, "SELECT label IS NULL FROM raw_values WHERE id = 9") == std::optional<std::int64_t>(0);
        Samples::checklist().check(isNullAndEmptyDistinct, "monostate 绑成真正的 SQL NULL，与空串在 IS NULL 上区分得开");

        // 少给参数：不失败就会让 WHERE 静默变成永假；多给参数：多出来的取值没有任何位置可绑
        const bool isFewRejected = connection.execute("SELECT id FROM raw_values WHERE id = ?", std::vector<Database::DatabaseValue>{}) == nullptr &&
                                   connection.lastError().find("参数数量不匹配") != std::string::npos;
        const bool isManyRejected =
                connection.execute("SELECT id FROM raw_values", std::vector<Database::DatabaseValue>{std::int64_t{1}}) == nullptr &&
                connection.lastError().find("参数数量不匹配") != std::string::npos;
        Samples::checklist().check(isFewRejected && isManyRejected, "参数个数与占位符个数不一致时一律拒绝，并给出中文的「参数数量不匹配」");

        // 容器类型无法映射成单个标量参数：正确用法是展开成多个占位符（方言层的 IN 已这么做）
        const bool isContainerRejected =
                connection.execute("SELECT ?", std::vector<Database::DatabaseValue>{std::vector<std::string>{"甲", "乙"}}) == nullptr &&
                connection.lastError().find("容器类型") != std::string::npos && connection.lastError().find("List") != std::string::npos;
        Samples::checklist().check(isContainerRejected, "容器类型参数被明确拒绝，原因里带着值的类型名而不是静默忽略");

        const std::unique_ptr<Database::DatabaseResult> brokenResult = connection.execute("SELECT * FROM no_such_table_here");
        Samples::checklist().check(brokenResult == nullptr && containsChinese(connection.lastError()) &&
                                           connection.lastError().find("no such table") != std::string::npos &&
                                           connection.lastError().find("错误码") != std::string::npos,
                                   "坏 SQL 以 nullptr + 「中文动作说明 + 引擎原文 + 错误码」暴露，不抛不崩");

        // 一次调用只接受一条语句：半执行比整次失败更难排查，第一条建表也不许留下
        const std::unique_ptr<Database::DatabaseResult> multiStatementResult =
                connection.execute("CREATE TABLE pairs (leftValue INTEGER); INSERT INTO pairs VALUES (1, 2);");
        const std::string multiStatementError = connection.lastError();
        const bool        isMultiStatementRejected =
                multiStatementResult == nullptr && containsChinese(multiStatementError) && connection.execute("SELECT * FROM pairs") == nullptr;
        Samples::checklist().check(isMultiStatementRejected, "多语句脚本整次被拒，第一条建表也没有执行");

        // 未连接时绝不把空句柄交给 SQLite：前置条件检查给出中文说明
        Database::SqliteConnection idleConnection(Database::ConnectionConfig::sqliteDefault());
        Samples::checklist().check(idleConnection.execute("SELECT 1") == nullptr && idleConnection.lastError().find("未连接") != std::string::npos &&
                                           !idleConnection.beginTransaction() && idleConnection.lastError().find("未连接") != std::string::npos,
                                   "未连接的 SQLite 连接在执行与事务入口上一律拒绝，并给出中文前置条件说明");

        // 同一条参数化接口在 Redis 上由基类默认实现给出「暂不支持」的中文提示（不需要服务端即可验证）
        Database::RedisConnection redisStub(Database::ConnectionConfig::redisDefault());
        const bool isRedisParameterizedRejected =
                redisStub.execute("GET ?", std::vector<Database::DatabaseValue>{std::int64_t{1}}) == nullptr &&
                redisStub.lastError().find("暂不支持参数化查询") != std::string::npos && redisStub.lastError().find("Redis") != std::string::npos;
        Samples::checklist().check(isRedisParameterizedRejected, "Redis 没有参数绑定实现时不退化成「按 NULL 执行」，而是给出中文提示");

        // 约束违例经 ORM 折成模块异常类型：能被 Base::Exception 一句网住，且库里一行都没多
        OrmQuery<AccountRow> beforeQuery(pool, Database::DatabaseType::Sqlite);
        const std::int64_t   countBefore = beforeQuery.count();
        bool                 isExceptionAsExpected = false;
        try
        {
            OrmQuery<AccountRow> duplicating(pool, Database::DatabaseType::Sqlite);
            static_cast<void>(duplicating.insert(
                    AccountRow{.id = 700, .name = "第一次", .balance = 1.0, .note = std::nullopt, .active = true}));
            OrmQuery<AccountRow> conflicting(pool, Database::DatabaseType::Sqlite);
            static_cast<void>(conflicting.insert(
                    AccountRow{.id = 700, .name = "重复主键", .balance = 2.0, .note = std::nullopt, .active = true}));
        }
        catch (const Database::QueryExecutionException &caught)
        {
            isExceptionAsExpected = containsChinese(caught.what()) && std::string_view(caught.what()).find("语句执行失败") != std::string_view::npos;
        }
        catch (const std::exception &caught)
        {
            LOG_ERROR_FMT("约束违例抛出了预期之外的异常类型：{}", caught.what());
        }
        OrmQuery<AccountRow> afterQuery(pool, Database::DatabaseType::Sqlite);
        Samples::checklist().check(isExceptionAsExpected && afterQuery.count() == countBefore + 1,
                                   "重复主键经 ORM 抛出 QueryExecutionException（中文原因），失败的那一行没有落库");
    }

    // ========================================================================
    // 9. 连接池
    // ========================================================================

    void demonstrateConnectionPool(Database::ConnectionPool &pool)
    {
        // 池上限 2：前两条借走后第三条应当被上限拦住，非阻塞路径立刻给出空
        Database::PooledConnection first  = pool.acquire();
        Database::PooledConnection second = pool.acquire();
        Database::PooledConnection third  = pool.tryAcquire();
        Samples::checklist().check(static_cast<bool>(first) && static_cast<bool>(second) && !static_cast<bool>(third) &&
                                           pool.activeCount() == 2 && pool.idleCount() == 0 && pool.totalCount() == pool.activeCount() &&
                                           pool.totalCount() <= 2,
                                   "连接池按上限发放：借满两条后 tryAcquire() 立刻给空，统计自洽");

        Database::DatabaseConnection *returnedConnection = second.operator->();
        const bool                    isMoved            = static_cast<bool>(second);
        Database::PooledConnection    moved(std::move(second));
        Samples::checklist().check(isMoved && static_cast<bool>(moved) && !static_cast<bool>(second),
                                   "PooledConnection 的移动语义：目标持有连接，源对象变空不会重复归还");
        moved.release();
        Samples::checklist().check(!static_cast<bool>(moved) && pool.activeCount() == 1 && pool.idleCount() == 1,
                                   "release() 提前归还后包装器为空，活跃数换到空闲数");

        // LIFO 复用：刚归还的那条连接应当是下一次拿到的同一条，而不是每次都新建
        Database::PooledConnection reBorrowed = pool.acquire();
        Samples::checklist().check(static_cast<bool>(reBorrowed) && reBorrowed.operator->() == returnedConnection &&
                                           reBorrowed.operator->() != first.operator->(),
                                   "归还的那条连接按 LIFO 被下一次借用拿到，而不是每次都新建");
        reBorrowed.release();
        first.release();

        // 归还路径上的会话复位：上一笔没提交的事务不能串给下一个借用者
        Database::PoolConfig singleConnectionConfiguration;
        singleConnectionConfiguration.maximumPoolSize            = 1;
        singleConnectionConfiguration.acquireTimeoutMilliseconds = 5000;
        Database::ConnectionPool resetPool(makeSqliteFactory(":memory:"), singleConnectionConfiguration);

        Database::SqliteConnection *rawConnection = nullptr;
        std::int64_t                leftoversSeenByNextBorrower = -1;
        bool                      isReusedAcrossReturns         = false;
        {
            Database::PooledConnection borrowed = resetPool.acquire();
            rawConnection = dynamic_cast<Database::SqliteConnection *>(borrowed.operator->());
            if (rawConnection != nullptr)
            {
                static_cast<void>(rawConnection->execute(kCreateRawTableSql));
                static_cast<void>(rawConnection->beginTransaction());
                static_cast<void>(rawConnection->execute("INSERT INTO raw_values (id, label) VALUES (1, 'leftover')"));
            }
            // 带着未提交的事务归还：池在放回空闲栈之前必须把会话复位
        }
        {
            const Database::PooledConnection next = resetPool.acquire();
            isReusedAcrossReturns = next.operator->() == rawConnection;
            leftoversSeenByNextBorrower = scalarInteger(*next, "SELECT COUNT(*) FROM raw_values").value_or(-1);
        }
        Samples::checklist().check(isReusedAcrossReturns && leftoversSeenByNextBorrower == 0,
                                   "归还时池调 resetSessionState()：未提交的事务被滚掉，半成品没有串给下一个借用者");

        // 上限被占满时另起线程阻塞等待；归还动作必须把它叫醒并把手里的连接直接交给它
        Database::PooledConnection held = resetPool.acquire();
        std::atomic<bool>          isWaiterServed{false};
        std::atomic<bool>          isWaiterHoldsConnection{false};
        std::thread                waiter(
                [&resetPool, &isWaiterServed, &isWaiterHoldsConnection]
                {
                    Database::PooledConnection borrowed = resetPool.acquire();
                    isWaiterHoldsConnection.store(static_cast<bool>(borrowed), std::memory_order_relaxed);
                    isWaiterServed.store(true, std::memory_order_release);
                });
        const bool isWaiterQueued = Samples::waitUntil([&resetPool]
                                                       {
                                                           return resetPool.waitingCount() > 0;
                                                       },
                                                       std::chrono::seconds{5}, std::chrono::milliseconds{5});
        held.release();
        const bool isServedInTime = Samples::waitUntil([&isWaiterServed]
                                                       {
                                                           return isWaiterServed.load(std::memory_order_acquire);
                                                       },
                                                       std::chrono::seconds{10}, std::chrono::milliseconds{5});
        waiter.join();
        Samples::checklist().check(isWaiterQueued && isServedInTime && isWaiterHoldsConnection.load(),
                                   "归还一条连接让排队的下一个借用者进得来（不是等自己的获取超时）");

        // 上限 0 的池：一个连接都不许建，只能按超时给出空，而不是挂死或崩
        Database::PoolConfig emptyConfiguration;
        emptyConfiguration.maximumPoolSize            = 0;
        emptyConfiguration.acquireTimeoutMilliseconds = 80;
        Database::ConnectionPool exhaustedPool(makeSqliteFactory(":memory:"), emptyConfiguration);
        Samples::checklist().check(!static_cast<bool>(exhaustedPool.acquire()) && exhaustedPool.totalCount() == 0,
                                   "maximumPoolSize 为 0 时 acquire() 按超时给出空连接，不挂死也不崩");
    }

    // ========================================================================
    // 10. 异步执行链路
    // ========================================================================

    /// 异步探针的观测结果：写在循环线程上，主线程等标记置起后才读
    struct AsyncProbeResult
    {
        std::int64_t      insertedRows{-1};             ///< insertAsync 回报的影响行数
        std::int64_t      countedRows{-1};              ///< countAsync 数到的行数
        std::size_t       fetchedRows{0};               ///< toListAsync 映射回来的行数
        bool              isPoolConnectionUsable{false};///< acquireAsync 交出的连接能不能用
        std::thread::id   resumedThreadId{};            ///< 协程最后一次恢复所在的线程
        std::string       errorText;                    ///< 协程内未能折成结论的异常原文
        std::atomic<bool> isFinished{false};            ///< 观测值写完由这枚标记 release 出去
    };

    Core::Task<> runAsyncProbe(Database::ConnectionPool &pool, Database::AsyncExecutor &executor, Core::EventLoop &loop,
                               AsyncProbeResult &outcome)
    {
        try
        {
            OrmQuery<AccountRow> insertQuery(pool, Database::DatabaseType::Sqlite);
            insertQuery.useAsyncExecutor(executor);
            outcome.insertedRows = co_await insertQuery.insertAsync(
                    AccountRow{.id = 21, .name = "异步写入", .balance = 1.5, .note = std::string("异步"), .active = true}, loop);

            OrmQuery<AccountRow> countQuery(pool, Database::DatabaseType::Sqlite);
            countQuery.useAsyncExecutor(executor);
            outcome.countedRows = co_await countQuery.countAsync(loop);

            OrmQuery<AccountRow> listQuery(pool, Database::DatabaseType::Sqlite);
            listQuery.useAsyncExecutor(executor);
            outcome.fetchedRows = (co_await listQuery.orderBy(Database::Queryable::asc("id")).toListAsync(loop)).size();

            Database::PooledConnection borrowed = co_await pool.acquireAsync(loop);
            outcome.isPoolConnectionUsable =
                    static_cast<bool>(borrowed) && scalarInteger(*borrowed, "SELECT COUNT(*) FROM \"sample accounts\"") == std::optional<std::int64_t>(1);
            outcome.resumedThreadId = std::this_thread::get_id();
        }
        catch (const std::exception &caught)
        {
            // 异常绝不逃出协程：留在观测结果里，由主线程折成一条失败结论
            outcome.errorText = caught.what();
        }
        outcome.isFinished.store(true, std::memory_order_release);
        co_return;
    }

    void demonstrateAsyncSurface(const TemporaryDatabaseFile &databaseFile)
    {
        Database::PoolConfig poolConfiguration;
        // 两条连接：一条给工作线程上的异步语句，另一条留给主线程的同步读回
        poolConfiguration.maximumPoolSize = 2;
        Database::ConnectionPool       pool(makeSqliteFactory(databaseFile.utf8Path()), poolConfiguration);
        Database::AsyncExecutor        executor{2};
        std::string                    errorText;
        if (!Database::Queryable::SchemaMigrator::createTable<AccountRow>(pool, true, &errorText))
        {
            Samples::checklist().check(false, "异步自检的前置建表失败：" + errorText);
            return;
        }

        Core::EventLoop loop;
        std::thread     loopThread([&loop]
        {
            loop.run();
        });
        const bool isLoopRunning = Samples::waitUntil([&loop]
                                                      {
                                                          return loop.isRunning();
                                                      },
                                                      std::chrono::seconds{5}, std::chrono::milliseconds{5});

        AsyncProbeResult outcome;
        auto             probeTask = runAsyncProbe(pool, executor, loop, outcome);
        loop.scheduler().scheduleRemote(probeTask.handle());
        const bool isProbeFinished = isLoopRunning && Samples::waitUntil([&outcome]
                                                                         {
                                                                             return outcome.isFinished.load(std::memory_order_acquire);
                                                                         },
                                                                         std::chrono::seconds{20}, std::chrono::milliseconds{5});

        loop.stop();
        loopThread.join();

        Samples::checklist().check(isProbeFinished && outcome.errorText.empty(),
                                   "异步链路的探针在时限内跑完且协程里没有异常（没有任何一次 await 挂死）");
        if (!outcome.errorText.empty())
        {
            LOG_ERROR_FMT("异步探针抛出的异常：{}", outcome.errorText);
        }
        Samples::checklist().check(outcome.insertedRows == 1 && outcome.countedRows == 1 && outcome.fetchedRows == 1,
                                   "insertAsync / countAsync / toListAsync 的结果互相自洽（一行写入、数到一行、映射回一行）");
        Samples::checklist().check(outcome.isPoolConnectionUsable, "acquireAsync() 交出的连接真的可用，且能读到异步写入的那一行");
        // 公开面没有承诺「异步链路在哪条线程上恢复」，因此这里只记录不断言：
        // 若落在工作线程上，调用方就不能在 await 之后直接碰循环持有的对象——这条契约该由框架先定
        LOG_INFO_FMT("异步探针的恢复线程与事件循环线程{}一致",
                     outcome.resumedThreadId == loopThread.get_id() ? "" : "不");

        // 异步写完必须用同步查询读回才算「数据真的落库」
        OrmQuery<AccountRow> syncRead(pool, Database::DatabaseType::Sqlite);
        const std::optional<AccountRow> written = syncRead.where(kAccountIdColumn == std::int64_t{21}).first();
        Samples::checklist().check(written.has_value() && written->name == "异步写入" && written->balance == 1.5 &&
                                           written->note == std::optional<std::string>("异步"),
                                   "异步写入的行由同步查询逐列读回");
    }

    // ========================================================================
    // 11 / 12. 外部服务端（按环境变量门控）
    // ========================================================================

    /// @return true 驱动已编译且 ASYN_MYSQL_TEST_PASSWORD 已设置
    bool isMySqlConfigured()
    {
        return kMySqlDriverCompiled && !Platform::ProcessInfo::environmentVariable(kMySqlPasswordVariableName).value_or(std::string{}).empty();
    }

    /// @return true 驱动已编译且 ASYN_REDIS_TEST_PASSWORD 已设置
    bool isRedisConfigured()
    {
        return kRedisDriverCompiled && !Platform::ProcessInfo::environmentVariable(kRedisPasswordVariableName).value_or(std::string{}).empty();
    }

    Database::ConnectionConfig makeMySqlConfigurationFromEnvironment()
    {
        Database::ConnectionConfig configuration = Database::ConnectionConfig::mySqlDefault();
        configuration.host     = readEnvironmentText(kMySqlHostVariableName, kDefaultMySqlHost);
        configuration.port     = readEnvironmentPort(kMySqlPortVariableName, kDefaultMySqlPort);
        configuration.userName = readEnvironmentText(kMySqlUserVariableName, kDefaultMySqlUserName);
        configuration.password = Platform::ProcessInfo::environmentVariable(kMySqlPasswordVariableName).value_or(std::string{});
        configuration.database = readEnvironmentText(kMySqlDatabaseVariableName, kDefaultMySqlDatabaseName);
        return configuration;
    }

    Database::ConnectionConfig makeRedisConfigurationFromEnvironment()
    {
        Database::ConnectionConfig configuration = Database::ConnectionConfig::redisDefault();
        configuration.host     = readEnvironmentText(kRedisHostVariableName, kDefaultRedisHost);
        configuration.port     = readEnvironmentPort(kRedisPortVariableName, kDefaultRedisPort);
        configuration.userName = readEnvironmentText(kRedisUserVariableName, "");
        configuration.password = Platform::ProcessInfo::environmentVariable(kRedisPasswordVariableName).value_or(std::string{});
        configuration.database = readEnvironmentText(kRedisDatabaseVariableName, kDefaultRedisKeyspace);
        return configuration;
    }

    /**
     * @brief MySQL 真机自检：只在 ASYN_MYSQL_TEST_PASSWORD 已设置时运行
     * @details 日志与表名里都不出现主机、用户名与口令；表名带进程号，重复运行与并行跑都不互撞
     */
    void demonstrateMySqlIntegration()
    {
        const Database::ConnectionConfig configuration = makeMySqlConfigurationFromEnvironment();
        Database::MySqlConnection        connection(configuration);
        if (!connection.connect())
        {
            // 环境变量给了但连不上：这是真机环境的故障，如实记一条失败并把原因写进日志（不含凭据）
            Samples::checklist().check(false, "MySQL 建连失败（原因见上一行日志，含客户端库原文与错误码）");
            LOG_ERROR(connection.lastError());
            return;
        }
        Samples::checklist().check(connection.isConnected() && !connection.serverVersion().empty(),
                                   "MySQL 真机建连成功并读到服务端版本号");

        const Database::MySqlDialect dialect;
        const std::string            tableName = dialect.quoteIdentifier("asyn_sample_mysql_" + std::to_string(Platform::ProcessInfo::currentProcessId()));

        static_cast<void>(connection.execute("DROP TABLE IF EXISTS " + tableName));
        const bool isCreated = connection.execute("CREATE TABLE " + tableName +
                                                  " (`id` BIGINT PRIMARY KEY, `name` VARCHAR(191) NOT NULL, `note` VARCHAR(191) NULL)") != nullptr;
        const std::vector<Database::DatabaseValue> insertParameters{std::int64_t{1}, std::string("O'Brien -- 中文"), std::monostate{}};
        const std::unique_ptr<Database::DatabaseResult> insertReceipt =
                connection.execute("INSERT INTO " + tableName + " (`id`, `name`, `note`) VALUES (?, ?, ?)", insertParameters);

        bool isRoundTripCorrect = false;
        if (insertReceipt != nullptr)
        {
            const std::vector<Database::DatabaseValue> selectParameters{std::int64_t{1}};
            if (const std::unique_ptr<Database::DatabaseResult> selection =
                        connection.execute("SELECT `name`, `note` IS NULL FROM " + tableName + " WHERE `id` = ?", selectParameters))
            {
                if (selection->next())
                {
                    isRoundTripCorrect = cellAs<std::string>(*selection, std::size_t{0}) == std::optional<std::string>("O'Brien -- 中文") &&
                                         cellAs<std::int64_t>(*selection, std::size_t{1}) == std::optional<std::int64_t>(1);
                }
            }
        }
        Samples::checklist().check(insertReceipt != nullptr && insertReceipt->affectedRowCount() == 1,
                                   "MySQL 写回执给出语句级影响行数（SQLite 之外第二个驱动也覆盖到）");
        Samples::checklist().check(isCreated && isRoundTripCorrect, "MySQL 参数化写入与读回：注入文本原样往返，NULL 仍不是空串");

        // 事务：回滚不留痕、提交才可见（连接级入口，语句文本同样取自 MySqlDialect）
        static_cast<void>(connection.execute("INSERT INTO " + tableName + " (`id`, `name`) VALUES (2, '事务外的行')"));
        const bool isRolledBack = connection.beginTransaction() &&
                                  connection.execute("INSERT INTO " + tableName + " (`id`, `name`) VALUES (3, '回滚行')") != nullptr &&
                                  connection.rollback();
        Samples::checklist().check(isRolledBack && scalarInteger(connection, "SELECT COUNT(*) FROM " + tableName) == std::optional<std::int64_t>(2),
                                   "MySQL 回滚后表里只剩事务外已提交的两行");

        static_cast<void>(connection.beginTransaction());
        static_cast<void>(connection.execute("INSERT INTO " + tableName + " (`id`, `name`) VALUES (4, '提交行')"));
        const bool isMySqlCommitEffective = connection.commit() &&
                                            scalarInteger(connection, "SELECT COUNT(*) FROM " + tableName) == std::optional<std::int64_t>(3);

        // 负向：重复主键必须给出「中文动作说明 + 服务端原文 + 错误码」而不是崩掉
        const std::unique_ptr<Database::DatabaseResult> duplicateReceipt =
                connection.execute("INSERT INTO " + tableName + " (`id`, `name`) VALUES (1, '重复主键')");
        const std::string duplicateError = connection.lastError();
        Samples::checklist().check(isMySqlCommitEffective && duplicateReceipt == nullptr && containsChinese(duplicateError) &&
                                           duplicateError.find("错误码") != std::string::npos,
                                   "MySQL 提交让第三行可见；重复主键以 nullptr + 中文原因（含错误码）暴露而不是崩掉");

        static_cast<void>(connection.execute("DROP TABLE IF EXISTS " + tableName));
        connection.disconnect();
    }

    /**
     * @brief Redis 真机自检：只在 ASYN_REDIS_TEST_PASSWORD 已设置时运行
     * @details 只碰自己带进程号前缀的键、只 DEL 自己的键，绝不 FLUSHDB；键空间默认 15，不混进工作库
     */
    void demonstrateRedisIntegration()
    {
        const Database::ConnectionConfig configuration = makeRedisConfigurationFromEnvironment();
        Database::RedisConnection        connection(configuration);
        if (!connection.connect())
        {
            Samples::checklist().check(false, "Redis 建连失败（原因见上一行日志，含 hiredis 原文）");
            LOG_ERROR(connection.lastError());
            return;
        }

        std::vector<std::string> createdKeys;
        const auto               makeKey = [&createdKeys](const std::string_view suffix)
        {
            std::string key = "asyngyanis:sample:" + std::to_string(Platform::ProcessInfo::currentProcessId()) + ":";
            key += suffix;
            createdKeys.push_back(key);
            return key;
        };

        const std::string probeKey  = makeKey("probe");
        const std::string secondKey = makeKey("second");
        // 值里带内嵌 '\0'：命令按「指针 + 长度」送出，截断就会在这里露出来
        const std::string binaryValue("ab\0cd", 5);

        bool isPingOkay = false;
        if (const std::unique_ptr<Database::DatabaseResult> ping = connection.executeCommand({"PING"}))
        {
            isPingOkay = cellAs<std::string>(*ping, std::size_t{0}) == std::optional<std::string>("PONG");
        }
        Samples::checklist().check(isPingOkay, "Redis 真机建连（含认证与键空间选择）后 PING 给出 PONG");

        const std::vector<std::string_view> setArguments{"SET", probeKey, binaryValue};
        const std::vector<std::string_view> getArguments{"GET", probeKey};
        bool isValueRoundTripOkay = connection.executeCommand(setArguments) != nullptr;
        if (isValueRoundTripOkay)
        {
            if (const std::unique_ptr<Database::DatabaseResult> reply = connection.executeCommand(getArguments))
            {
                const std::optional<std::string> stored = cellAs<std::string>(*reply, std::size_t{0});
                isValueRoundTripOkay = stored.has_value() && *stored == binaryValue && stored->size() == 5;
            }
            else
            {
                isValueRoundTripOkay = false;
            }
        }
        // 缺失的键给 nil：那是「没有值」，与「值是空串」不是一回事
        bool isMissingKeyEmpty = false;
        if (const std::unique_ptr<Database::DatabaseResult> missing = connection.executeCommand({"GET", makeKey("absent")}))
        {
            isMissingKeyEmpty = missing->isEmpty() && std::holds_alternative<std::monostate>(missing->getValue(std::size_t{0}));
        }
        Samples::checklist().check(isValueRoundTripOkay && isMissingKeyEmpty,
                                   "Redis 按参数数组存取：内嵌 '\\0' 的载荷逐字节回读，缺失的键给空结果集而不是空串");

        // 管道：一次 flush 收齐全部回复，顺序与登记顺序严格一致
        const bool isPipelined = connection.pipelineCommand("SET " + secondKey + " alpha") && connection.pipelineCommand("GET " + secondKey) &&
                                 connection.pipelineCommand("GET");
        const std::vector<std::unique_ptr<Database::DatabaseResult> > replies = connection.flushPipeline();
        bool isPipelineOrdered = false;
        if (replies.size() == 3 && replies[0] != nullptr && replies[1] != nullptr && replies[2] != nullptr)
        {
            isPipelineOrdered = cellAs<std::string>(*replies[0], std::size_t{0}) == std::optional<std::string>("OK") &&
                                cellAs<std::string>(*replies[1], std::size_t{0}) == std::optional<std::string>("alpha") &&
                                !replies[2]->lastError().empty();
        }
        Samples::checklist().check(isPipelined && isPipelineOrdered,
                                   "管道一次 flush 按登记顺序给出三条回复，其中出错那条的原因留在它自己的结果上");

        // 负向：服务端 error 回复以 nullptr + 中文说明暴露（含服务端原文），连接本身仍可用
        const bool isCommandErrorReported = connection.execute("GET") == nullptr &&
                                            connection.lastError().find("Redis 服务器返回错误") != std::string::npos &&
                                            containsChinese(connection.lastError());
        Samples::checklist().check(isCommandErrorReported && connection.executeCommand({"PING"}) != nullptr,
                                   "Redis 命令报错给出中文前缀，且失败不会把连接本身弄坏");

        for (const std::string &key: createdKeys)
        {
            static_cast<void>(connection.executeCommand({"DEL", key}));
        }

        // 键空间切换：切到 0 号库后刚写的键必须消失，切回配置的库又看得见
        static_cast<void>(connection.executeCommand({"SET", probeKey, "after-cleanup"}));
        const int configuredKeyspace = readEnvironmentInteger(kRedisDatabaseVariableName, kDefaultRedisKeyspaceIndex);
        bool      isKeyspaceSwitched = false;
        if (connection.selectDatabase(0))
        {
            const bool isHiddenInDefaultKeyspace = readIntegerReply(connection, {"EXISTS", probeKey}) == std::optional<std::int64_t>(0);
            if (connection.selectDatabase(configuredKeyspace))
            {
                isKeyspaceSwitched = isHiddenInDefaultKeyspace &&
                                     readIntegerReply(connection, {"EXISTS", probeKey}) == std::optional<std::int64_t>(1);
            }
        }
        Samples::checklist().check(isKeyspaceSwitched, "selectDatabase() 真的换了键空间：同一个键在 0 号库里不可见");
        static_cast<void>(connection.executeCommand({"DEL", probeKey}));
        connection.disconnect();
    }
} // namespace

int main()
{
    Samples::setupConsoleLogging();
    LOG_INFO("=== Database 子系统示例开始 ===");

    const TemporaryDatabaseFile rawDatabaseFile{"raw"};
    const TemporaryDatabaseFile factoryDatabaseFile{"factory"};
    const TemporaryDatabaseFile ormDatabaseFile{"orm"};
    const TemporaryDatabaseFile asyncDatabaseFile{"async"};

    Database::PoolConfig poolConfiguration;
    // 上限 2：一条给事务或本地事务持有，另一条留给「站在旁观连接上」的读回
    poolConfiguration.maximumPoolSize            = 2;
    poolConfiguration.acquireTimeoutMilliseconds = 3000;
    auto samplePool = std::make_unique<Database::ConnectionPool>(makeSqliteFactory(ormDatabaseFile.utf8Path()), poolConfiguration);

    runStep("SQLite 文件库", [&rawDatabaseFile] { demonstrateSqliteFileDatabase(rawDatabaseFile); });
    runStep("SQLite 内存库", [] { demonstrateSqliteInMemoryDatabase(); });
    runStep("连接工厂与类型枚举", [&factoryDatabaseFile] { demonstrateFactoryAndTypes(factoryDatabaseFile); });
    runStep("SQL 方言层", [] { demonstrateDialectLayer(); });
    runStep("ORM 与查询构建", [&samplePool] { demonstrateObjectRelationalMapping(*samplePool); });
    runStep("事务", [&samplePool] { demonstrateTransactions(*samplePool); });
    runStep("二进制列", [&samplePool] { demonstrateBinaryColumn(*samplePool); });
    runStep("参数绑定与拒绝路径", [&samplePool] { demonstrateParameterBindingAndRejections(*samplePool); });
    runStep("连接池", [&samplePool] { demonstrateConnectionPool(*samplePool); });
    runStep("异步执行链路", [&asyncDatabaseFile] { demonstrateAsyncSurface(asyncDatabaseFile); });

    runGatedStep("MySQL 真机", isMySqlConfigured(), "未设置环境变量 ASYN_MYSQL_TEST_PASSWORD（凭据只从环境进来，仓库零明文）",
                 [] { demonstrateMySqlIntegration(); });
    runGatedStep("Redis 真机", isRedisConfigured(), "未设置环境变量 ASYN_REDIS_TEST_PASSWORD（凭据只从环境进来，仓库零明文）",
                 [] { demonstrateRedisIntegration(); });

    // 连接必须先关再删文件：Windows 上打开着的文件删不掉
    samplePool.reset();
    LOG_INFO("=== Database 子系统示例结束 ===");
    return Samples::finishSample("database_demo");
}
