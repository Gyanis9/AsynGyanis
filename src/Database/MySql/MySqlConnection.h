/**
 * @file MySqlConnection.h
 * @brief MySQL / MariaDB 数据库连接实现
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Database/Common/DatabaseConnection.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>

// MySQL C API 的全局 C 类型前置声明集中写在本头的全局作用域（全项目只此一处）：
// 只有 .cpp 才包含 <mysql/mysql.h>，避免第三方 C 头顺着包含链传染给所有使用方。
// MySqlResult.h 通过包含本头复用下面这几行声明，不得重复声明。
//
// 声明形式必须与真实头文件完全一致：libmysqlclient 8.x 起连接句柄与结果集都是
// 「结构体标签即类型名」（struct MYSQL / struct MYSQL_RES，与 mysql/client_plugin.h 里的
// struct MYSQL; 同源），而不是 5.x 时代的 typedef struct st_mysql MYSQL。
// 按旧写法声明 st_mysql 会让同一个名字在 .cpp 里被重定义成不同类型（C2371），
// 进而让每个 mysql_* 调用的句柄形参都无法匹配（C2664）——这正是真实驱动此前从未编译过
// 才得以隐藏的错误。此处只前置声明，不引入任何第三方头
struct MYSQL;
struct MYSQL_RES;
struct MYSQL_STMT;

using MYSQL_ROW = char **; ///< 一行数据，对应 mysql.h 的 typedef char **MYSQL_ROW：每列一个 char*，该列为 SQL NULL 时元素为空指针

namespace AsynGyanis::Database
{
    /**
     * @brief MySQL / MariaDB 数据库连接
     *
     * @details 封装 libmysqlclient（或 MariaDB Connector/C，两者的 C API 同名）实现 DatabaseConnection 抽象接口。
     *          本驱动是可选编译的：CMake 找到 libmysqlclient 时定义 DATABASE_HAS_MYSQL 编出真实实现，
     *          否则退化成一个明确报错的桩——connect() 恒为 false，并把「当前构建未编译 MySQL 驱动」
     *          写进 lastError()，调用方不会把「什么都没做」误当成成功。
     *
     * ConnectionConfig 的五个字段在本驱动全部有效：
     * - host / port：连接地址。port 为 0 时由客户端库回落到编译期默认端口 3306；
     *   POSIX 平台上 host 填 "localhost" 且未指定 unix_socket 时，客户端库走默认 Unix 域套接字，
     *   此时 TCP 端口不参与建连，需要强制走 TCP 请填 127.0.0.1。
     * - userName / password：认证信息，按长度原样交给 mysql_real_connect，本类不做任何转义。
     * - database：登录后默认选中的库；为空串表示不选任何库（此时只能用不依赖库名的命令）。
     *
     * 超时策略：基类的 connectTimeout() / queryTimeout() 单位是毫秒，而 MySQL 客户端的
     *          MYSQL_OPT_CONNECT_TIMEOUT / MYSQL_OPT_READ_TIMEOUT / MYSQL_OPT_WRITE_TIMEOUT
     *          三个选项接受的单位是秒（参数类型 unsigned int，MySQL 5.6+/8.x 与 MariaDB Connector/C 3.x 一致，
     *          公开 API 里没有对应的微秒选项），因此由 connect() 向上取整换算后逐条下发；
     *          非正值折算成 0，含义是「不超时」。这三个选项只在握手之前被客户端库读取一次，
     *          没有运行期修改的公开手段，所以 connect() 之后再调用 setConnectTimeout() / setQueryTimeout()
     *          不会影响当前会话，必须 disconnect() + connect() 才生效。
     *
     * 字符集：握手前用 MYSQL_SET_CHARSET_NAME 定为 utf8mb4，一次协商到位，避免出现
     *        「已连上但仍是服务端默认 latin1」的中间态（连上后再 mysql_set_character_set 要多一次往返）。
     *        服务端不认识该字符集时整次连接失败并给出明确原因，比默默按 latin1 收数据、让中文列变乱码更可取。
     *
     * 命令边界：一次 execute() 只发一条语句（未启用 CLIENT_MULTI_STATEMENTS），
     *          拼接的多余语句会被服务端报语法错误，整条命令一条都不执行。
     *
     * 结果集：一律走 mysql_store_result 把整份数据预读进客户端内存，因此 execute() 交出的 MySqlResult
     *        不引用本连接的任何内存，可以比连接对象活得更久；代价是大结果集会等额占用内存。
     *        参数化执行（mysql_stmt_* 二进制协议）同样预读：那里由 MySqlStatementResult 承载快照，
     *        两条路径的结果集语义一致，调用方只依赖 DatabaseResult 接口。
     *        写语句交出的空回执携带语句级影响行数（affectedRowCount()），只读结果集按约定返回 0。
     *
     * 生命周期：构造（不分配句柄、不做 IO）→ connect() → execute() / 事务 → disconnect() → 析构。
     *          析构自动调用 disconnect()。本对象独占 MYSQL 句柄，拷贝或移动后的源对象析构时会
     *          重复 mysql_close，因此一律禁止。句柄本身不是线程安全的：一条连接同一时刻只能由一个线程使用，
     *          需要并发就每线程一条连接；多线程首次建连前请在进程启动处调用一次 mysql_library_init()
     *          （本类不隐式调用，避免与上层已有的初始化重复）。
     *
     * 错误处理：所有错误原因写入基类的 m_lastError，lastError() 沿用基类实现，本类不做重复覆写；
     *          面向使用者的文本一律中文，并带上客户端库原文与错误码。
     *          链路被服务端单方面断开（CR_SERVER_GONE_ERROR / CR_SERVER_LOST）时，execute() 的失败路径
     *          会顺手断开连接，调用方重连即可；isConnected() 不发 mysql_ping，不做任何网络往返。
     *
     * @code
     *   auto connection = DatabaseFactory::createMySql(ConnectionConfig::mySqlDefault());
     *   if (connection->connect())
     *   {
     *       auto result = connection->execute("SELECT id, name FROM users");
     *       while (result != nullptr && result->next())
     *       {
     *           const DatabaseValue name = result->getValue("name");
     *       }
     *   }
     * @endcode
     */
    class MySqlConnection : public DatabaseConnection
    {
    public:
        /**
         * @brief 使用配置构造 MySQL 连接，此阶段不分配句柄也不发生任何网络交互
         * @param configuration 连接配置；host/port 用于建连，userName/password 用于认证，
         *                      database 为登录后默认选中的库
         */
        explicit MySqlConnection(const ConnectionConfig &configuration);

        /**
         * @brief 析构时自动断开连接，释放底层 MYSQL 句柄
         */
        ~MySqlConnection() override;

        // 句柄所有权唯一：拷贝会让两个对象 mysql_close 同一个 MYSQL；
        // 移动会让源对象析构时再次 mysql_close（句柄已交给目标对象），
        // 因此拷贝与移动一律禁止（基类同样已删除，这里显式写清意图）。
        MySqlConnection(const MySqlConnection &) = delete;

        MySqlConnection &operator=(const MySqlConnection &) = delete;

        MySqlConnection(MySqlConnection &&) = delete;

        MySqlConnection &operator=(MySqlConnection &&) = delete;

        /**
         * @brief 连接 MySQL 服务
         * @details 重写 DatabaseConnection::connect()：与基类契约的差异与附加行为——
         *          1) 已连接时直接返回 true，保持幂等（在同一句柄上再次 mysql_real_connect 相当于隐式重连，
         *             会丢掉事务、会话变量等全部会话状态）；
         *          2) MYSQL 句柄在本方法内部才由 mysql_init 创建，构造阶段不留任何资源；
         *          3) 每次建连都重新读取 connectTimeout() / queryTimeout() 的当前值并按毫秒→秒换算下发
         *             （旧实现在构造函数里一次性下发，外部 setter 永远无效），同时下发 utf8mb4 字符集；
         *          4) host 为空视为配置错误，当场失败而不是交给客户端库报出难懂的底层错误；
         *          5) 任何一步失败都先摘好错误文本再释放句柄，绝不留下半开的句柄，也绝不读悬垂指针。
         *          桩构建（未编译 libmysqlclient）下本方法直接返回 false，并在 lastError() 给出缺失驱动的提示。
         * @return true 连接已建立（含默认库选择）
         * @return false 任一环节失败，原因（含客户端库原文与错误码）见 lastError()
         * @note 重连前修改超时设置即可生效；已连接状态下修改设置要等下一次 disconnect() + connect()
         */
        bool connect() override;

        /**
         * @brief 断开连接并释放底层 MYSQL 句柄
         * @details 重写 DatabaseConnection::disconnect()：与基类的差异——
         *          1) 只负责关句柄与复位状态，不清 m_lastError：调用方常在失败路径上先写好原因再断开，
         *             覆盖它就会把真正的根因丢掉；
         *          2) 已交出的 MySqlResult 自带全部数据（mysql_store_result 预读），断开后仍可正常读取；
         *          3) mysql_close 之后 mysql_error() 的返回值即失效，所以本方法内部不再读取任何错误文本，
         *             需要错误信息的失败路径必须「先取文本、再断开」。
         *          未连接且无残留句柄时是安全的空操作，析构函数会无条件调用本方法。
         * @note 断开后句柄被置空，下一次 connect() 会重新 mysql_init 并按最新的超时设置下发选项
         */
        void disconnect() override;

        /**
         * @brief 判断连接是否可用
         * @details 重写 DatabaseConnection::isConnected()：不做 mysql_ping 活性探测
         *          （一次网络往返的代价对高频查询不可接受），只同时校验基类的 m_isConnected 标志
         *          与句柄非空；两者不一致时按未连接处理。链路被对端单方面断开时这里仍会返回 true，
         *          真实失效由 execute() 的失败路径发现并顺带断开连接。
         * @return true 已连接且句柄有效
         */
        [[nodiscard]] bool isConnected() const override;

        // 引入基类的全部 execute 重载：本类声明了名为 execute 的成员，按 C++ 名字查找规则
        // 会隐藏基类的同名重载，加上这一行后通过具体对象也能调用全部版本
        using DatabaseConnection::execute;

        /**
         * @brief 执行一条 SQL 命令
         * @details 重写 DatabaseConnection::execute()：与基类约定的差异与附加约束——
         *          - 每次调用开头清空 m_lastError，成功调用不会残留上一轮的失败文本；
         *          - 命令文本按「指针 + 长度」交给 mysql_real_query，本身二进制安全，不要求零终止；
         *          - 命令为空或长度超出 unsigned long 上限时直接失败，不发送任何字节；
         *          - 有返回列时把整份结果预读成 MySqlResult；无返回列的写语句返回「执行成功的空回执」
         *           （非空指针，但 rowCount() 为 0，affectedRowCount() 为本条语句实际改动的行数），
         *           调用方只判 nullptr 即可区分失败与空结果；
         *          - 客户端报出连接级错误（CR_SERVER_GONE_ERROR / CR_SERVER_LOST）时顺手断开连接，
         *           因为该句柄已无法复用，重连即可；
         *          - 一次只发一条语句（未启用 CLIENT_MULTI_STATEMENTS），拼接的后续语句会被服务端判语法错误。
         * @param command SQL 文本，例如 "SELECT id, name FROM users"
         * @return std::unique_ptr<DatabaseResult> 结果集；失败返回 nullptr，原因见 lastError()
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> execute(std::string_view command) override;

        /**
         * @brief 执行一条带占位符的 SQL 命令，参数按位置绑定
         * @details 重写 DatabaseConnection::execute()：与基类默认实现（直接报「暂不支持」）不同，
         *          本驱动用 MySQL 的**预处理语句接口**（mysql_stmt_*）真正绑定参数，
         *          取值绝不拼进 SQL 文本，注入面因此彻底消失：
         *          1) mysql_stmt_init + mysql_stmt_prepare 把语句文本编译成服务端预处理语句；
         *          2) 参数个数必须与占位符个数严格相等，不等直接失败——MySQL 对未绑定的占位符
         *             会按 NULL 参与运算，少给参数会让条件静默变成永假（`WHERE id = NULL`），
         *             几乎不可能从结果上反推原因；
         *          3) 逐参数按 DatabaseValue 的备选选择 MYSQL_BIND 的 buffer_type，
         *             NULL 用 MYSQL_TYPE_NULL 表达（绑成空串会让 IS NULL 不再成立），
         *             容器类型（List / Hash）明确拒绝并给出中文原因（正确用法是展开成多个标量参数）；
         *          4) mysql_stmt_execute 之后：无返回列的写语句用 mysql_stmt_affected_rows 取影响行数，
         *             交出一个 0 行 0 列的 MySqlResult 写回执；有返回列的查询先 mysql_stmt_store_result
         *             把整份结果预读进客户端内存，再逐行转换成 MySqlStatementResult 快照
         *             （结果集因此不引用语句句柄，可以比连接活得更久）；
         *          5) 预处理语句句柄由本方法独占，所有返回路径（含失败路径）都保证 mysql_stmt_close。
         *
         * 与不带参数版本的差异：参数化路径走二进制协议，列值统一按字符串缓冲读取后
         * 再按列声明类型解析，因此取值映射（NULL→monostate、整数→int64_t、浮点→double、
         * DECIMAL 与其余类型→std::string）与文本协议路径完全一致，两条路径可互换使用。
         *
         * @param command 带 "?" 占位符的 SQL 文本，例如 "SELECT id FROM users WHERE age >= ?"
         * @param parameters 按占位符出现顺序排列的绑定参数，个数必须等于占位符个数
         * @return std::unique_ptr<DatabaseResult> 结果集；失败返回 nullptr，原因见 lastError()
         * @note 缺少服务端时的可用性：未连接、参数个数不匹配、命令为空、容器类型参数
         *       这几条判定都发生在发起任何网络往返之前或之后立即返回，因此不需要服务端即可验证
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> execute(std::string_view command, std::span<const DatabaseValue> parameters) override;

        /**
         * @brief 获取数据库类型
         * @details 重写 DatabaseConnection::databaseType()：恒定返回 DatabaseType::MySql，
         *          不依赖连接状态，桩构建下同样返回本类型。
         * @return DatabaseType DatabaseType::MySql
         */
        [[nodiscard]] DatabaseType databaseType() const override;

        /**
         * @brief 开始一个事务
         * @details 执行本方言（MySqlDialect）给出的开启语句 "START TRANSACTION"，是 MySQL 专有能力的
         *          便捷封装，不覆盖基类任何虚函数。语句文本取自方言而非硬编码，
         *          与 Transaction + 方言这条路径共用同一份语句来源，二者不会漂移。
         * @return true 事务已开启
         * @return false 未连接或语句被服务端拒绝，原因见 lastError()
         * @note 默认自动提交为 ON 时不需要显式开事务；本方法不会去改 autocommit 会话变量
         */
        bool beginTransaction();

        /**
         * @brief 提交当前事务
         * @details 执行本方言给出的提交语句 "COMMIT"，语句文本同样以方言为唯一来源。
         * @return true 提交成功
         * @return false 没有活动事务或未连接，原因见 lastError()
         */
        bool commit();

        /**
         * @brief 回滚当前事务
         * @details 执行本方言给出的回滚语句 "ROLLBACK"，与 commit() 一样属于事务控制便捷封装，
         *          语句文本同样以方言为唯一来源。
         * @return true 回滚成功
         * @return false 没有活动事务或未连接，原因见 lastError()
         */
        bool rollback();

        /**
         * @brief 获取服务端版本字符串
         * @details 走 mysql_get_server_info，它必须持有已建立连接的句柄，因此未连接时返回空串
         *          （这与 Sqlite 驱动不同：SQLite 是进程内引擎，版本随时可取）。
         * @return std::string 形如 "8.0.36" 的服务端版本号；未连接或桩构建下为空串
         */
        [[nodiscard]] std::string serverVersion() const;

        /**
         * @brief 获取底层 MYSQL 句柄，供需要直接使用 MySQL C API 的高级场景使用
         * @warning 句柄所有权始终属于本连接：调用方不得 mysql_close，也不得在连接销毁后继续使用；
         *          拿它去发命令会绕过本类的超时设置、错误处理与断链复位逻辑
         * @return MYSQL* 未连接（或桩构建）时为 nullptr
         */
        [[nodiscard]] MYSQL *nativeHandle() const noexcept
        {
            return m_mysqlHandle;
        }

    private:
        /**
         * @brief 采集客户端库的错误文本与错误码并写入 m_lastError
         * @param description 面向使用者的中文动作说明，例如「执行 SQL 命令失败」
         */
        void captureError(std::string_view description);

        /**
         * @brief 在握手前把超时与字符集选项下发到句柄
         * @details 读取 connectTimeout() / queryTimeout() 的当前毫秒值，换算成客户端需要的整秒。
         * @return true 全部选项都被客户端库接受
         * @return false 任一选项被拒（原因已写入 m_lastError），调用方应放弃本次连接而不是留下无超时会话
         */
        bool applyConnectionOptions();

        /**
         * @brief 采集预处理语句上的错误文本与错误码并写入 m_lastError
         * @details mysql_stmt_* 的错误状态挂在语句句柄上而不是连接句柄上，
         *          必须用 mysql_stmt_error / mysql_stmt_errno 取，用连接级接口会读到上一次
         *          连接操作的陈旧错误。文本同样必须先拷贝再关语句（mysql_stmt_close 会释放该缓冲）。
         * @param statement 出错的预处理语句句柄
         * @param description 面向使用者的中文动作说明，例如「执行预处理语句失败」
         */
        void captureStatementError(MYSQL_STMT *statement, std::string_view description);

        /**
         * @brief 绑定参数并执行一条已预处理的语句
         * @details 绑定缓冲区（参数个数、长度表、布尔与整数的落地位）都是本函数的局部变量，
         *          生命周期只覆盖到 mysql_stmt_execute 返回为止——客户端库正是在 execute 内部
         *          把这些缓冲区的内容写进网络包，因此局部存储是安全的，但也意味着
         *          「绑定」与「执行」必须成对出现在同一个函数里，不能把绑定结果留到函数外使用。
         *          执行失败时按连接级错误码判断链路是否已断（CR_SERVER_GONE_ERROR / CR_SERVER_LOST），
         *          是则顺手 disconnect()，让后续调用一致地看到「未连接」。
         * @param statement 已 prepare 成功的预处理语句句柄
         * @param parameters 按占位符出现顺序排列的绑定参数
         * @return true 参数个数匹配、绑定与执行都成功
         * @return false 任一步失败，原因（含错误码）见 lastError()
         */
        bool bindAndExecuteStatement(MYSQL_STMT *statement, std::span<const DatabaseValue> parameters);

        /**
         * @brief 把已执行并 store_result 的预处理语句的全部行预读成结果集快照
         * @details 先取一次列元数据（列名、声明类型、max_length），按每列的 max_length 分配取值缓冲区，
         *          再用 mysql_stmt_fetch 逐行取回并转换成 DatabaseValue。任何一列都比 max_length 长时
         *          （客户端库未按 STMT_ATTR_UPDATE_MAX_LENGTH 更新长度才会发生）按实际长度补取一次，
         *          绝不把截断的数据交给调用方。
         * @param statement 已 mysql_stmt_execute + mysql_stmt_store_result 成功的语句句柄
         * @return std::unique_ptr<DatabaseResult> 结果集快照；失败返回 nullptr，原因见 lastError()
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> materializePreparedResult(MYSQL_STMT *statement);

        MYSQL *m_mysqlHandle{nullptr}; ///< MySQL C API 连接句柄，本对象独占所有权，未连接时为 nullptr
    };

} // namespace AsynGyanis::Database
