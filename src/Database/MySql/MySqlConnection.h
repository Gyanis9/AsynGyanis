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
// struct MYSQL; 同源），写成 st_mysql 之类的别名会让同一个名字在 .cpp 里被重定义成
// 不同类型（C2371），进而让每个 mysql_* 调用的句柄形参都无法匹配（C2664）。
struct MYSQL;
struct MYSQL_RES;
struct MYSQL_STMT;

using MYSQL_ROW = char **; ///< 一行数据，对应 mysql.h 的 typedef char **MYSQL_ROW：每列一个 char*，该列为 SQL NULL 时元素为空指针

namespace AsynGyanis::Database
{
    /**
     * @brief MySQL / MariaDB 数据库连接
     *
     * @details 可选编译：找到 libmysqlclient 时编出真实实现，否则退化为「connect() 恒为 false、
     *          原因写进 lastError()」的桩。基类超时单位是毫秒而客户端选项只接受整秒（向上取整），
     *          且这三个选项只在握手前被读取一次，因此必须在 connect() 之前设置，否则对当前会话无效。
     *          一条连接同一时刻只能由一个线程使用；mysql_close 之后 mysql_error() 的返回值即失效。
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
         * @details 重写 DatabaseConnection::connect()：已连接时直接返回 true（再次 mysql_real_connect
         *          相当于隐式重连、会丢掉全部会话状态）；MYSQL 句柄在本方法内创建，每次建连都重新下发
         *          超时设置与 utf8mb4 字符集；host 为空当场失败，任何一步失败都先摘好错误文本再释放句柄。
         *          其余与基类一致；桩构建下直接返回 false 并在 lastError() 给出缺失驱动的提示。
         * @return true 连接已建立（含默认库选择）
         * @return false 任一环节失败，原因（含客户端库原文与错误码）见 lastError()
         * @note 重连前修改超时设置即可生效；已连接状态下修改设置要等下一次 disconnect() + connect()
         */
        bool connect() override;

        /**
         * @brief 断开连接并释放底层 MYSQL 句柄
         * @details 重写 DatabaseConnection::disconnect()：只关句柄与复位状态，不清 m_lastError（失败路径常
         *          先写好原因再断开）；已交出的 MySqlResult 自带全部数据，断开后仍可读取；mysql_close 之后
         *          mysql_error() 的返回值即失效，失败路径必须「先取文本、再断开」。其余与基类一致。
         * @note 断开后句柄被置空，下一次 connect() 会重新 mysql_init 并按最新的超时设置下发选项
         */
        void disconnect() override;

        /**
         * @brief 判断连接是否可用
         * @details 重写 DatabaseConnection::isConnected()：不做 mysql_ping 活性探测，只校验
         *          m_isConnected 标志与句柄非空，两者不一致时按未连接处理；链路被对端单方面断开时
         *          这里仍返回 true，真实失效由 execute() 的失败路径发现并顺带断开连接。
         * @return true 已连接且句柄有效
         */
        [[nodiscard]] bool isConnected() const override;

        // 引入基类的全部 execute 重载：本类声明了名为 execute 的成员，按 C++ 名字查找规则
        // 会隐藏基类的同名重载，加上这一行后通过具体对象也能调用全部版本
        using DatabaseConnection::execute;

        /**
         * @brief 执行一条 SQL 命令
         * @details 重写 DatabaseConnection::execute()：每次调用开头清空 m_lastError；命令按「指针 + 长度」
         *          交给 mysql_real_query（二进制安全），命令为空或超长时直接失败、不发送任何字节；有返回列时
         *          预读成 MySqlResult，无返回列返回非空的空回执；客户端报连接级错误时顺手断开；一次只发一条
         *          语句（未启用 CLIENT_MULTI_STATEMENTS）。其余与基类一致。
         * @param command SQL 文本，例如 "SELECT id, name FROM users"
         * @return std::unique_ptr<DatabaseResult> 结果集；失败返回 nullptr，原因见 lastError()
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> execute(std::string_view command) override;

        /**
         * @brief 执行一条带占位符的 SQL 命令，参数按位置绑定
         * @details 重写 DatabaseConnection::execute()：用预处理语句接口（mysql_stmt_*）真正绑定参数，
         *          取值绝不拼进 SQL 文本。与基类默认实现（直接报「暂不支持」）不同：参数个数必须与
         *          占位符个数严格相等（少给参数会让条件静默变成永假 `WHERE id = NULL`）；NULL 用
         *          MYSQL_TYPE_NULL 表达（绑成空串会让 IS NULL 不再成立）；容器类型明确拒绝；结果预读成
         *          不引用语句句柄、可活得比连接更久的快照。其余与基类一致。
         * @param command 带 "?" 占位符的 SQL 文本，例如 "SELECT id FROM users WHERE age >= ?"
         * @param parameters 按占位符出现顺序排列的绑定参数，个数必须等于占位符个数
         * @return std::unique_ptr<DatabaseResult> 结果集；失败返回 nullptr，原因见 lastError()
         * @note 未连接、参数个数不匹配、命令为空、容器类型参数这几条判定都不需要服务端即可验证
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> execute(std::string_view command, std::span<const DatabaseValue> parameters) override;

        /**
         * @brief 获取数据库类型
         * @details 重写 DatabaseConnection::databaseType()：恒定返回 DatabaseType::MySql，不依赖连接状态。
         * @return DatabaseType DatabaseType::MySql
         */
        [[nodiscard]] DatabaseType databaseType() const override;

        /**
         * @brief 开始一个事务
         * @details 执行本方言（MySqlDialect）给出的开启语句 "START TRANSACTION"，是 MySQL 专有能力的
         *          便捷封装；语句文本以方言为唯一来源，与 Transaction 路径共用，二者不会漂移。
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
         * @details 执行本方言给出的回滚语句 "ROLLBACK"，语句文本同样以方言为唯一来源。
         * @return true 回滚成功
         * @return false 没有活动事务或未连接，原因见 lastError()
         */
        bool rollback();

        /**
         * @brief 归还连接池时复位会话状态：把本类开着的事务滚掉
         * @details 残留的事务会跟着连接串给下一个借用者：对方的语句悄悄并进上一笔事务，
         *          行锁与元数据锁也一直被握到事务结束（可能永远不结束）为止。
         *          判定用本类记下的「事务是否开着」（beginTransaction 置位、commit/rollback
         *          清零）：MySQL 8 起 `struct MYSQL` 对使用方是不完整类型，读不到客户端库
         *          记录的服务端状态位，公共 C API 也没有对应的取值函数。
         * @note 与基类契约一致：不抛异常、幂等；未连接或本类没开过事务时不做任何事
         * @note **记账范围**是本类的事务入口：手工执行的 "START TRANSACTION"、把 autocommit
         *       关掉的会话不在这份记账里——那些是绕过连接对象自管的用法，本方法看不出它们
         */
        void resetSessionState() noexcept override;

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
         * @details 绑定缓冲区（参数个数、长度表、布尔与整数的落地位）都是本函数的局部变量，客户端库在
         *          mysql_stmt_execute 内部读取它们并写进网络包，因此「绑定」与「执行」必须成对出现在
         *          同一个函数里，绑定结果不能留到函数外使用。执行失败时按连接级错误码判断链路是否已断
         *          （CR_SERVER_GONE_ERROR / CR_SERVER_LOST），是则顺手 disconnect()。
         * @param statement 已 prepare 成功的预处理语句句柄
         * @param parameters 按占位符出现顺序排列的绑定参数
         * @return true 参数个数匹配、绑定与执行都成功
         * @return false 任一步失败，原因（含错误码）见 lastError()
         */
        bool bindAndExecuteStatement(MYSQL_STMT *statement, std::span<const DatabaseValue> parameters);

        /**
         * @brief 把已执行并 store_result 的预处理语句的全部行预读成结果集快照
         * @details 先取一次列元数据（列名、声明类型、max_length），按每列的 max_length 分配取值缓冲区，
         *          再用 mysql_stmt_fetch 逐行取回并转换成 DatabaseValue；任何一列比 max_length 长时按实际
         *          长度补取一次（客户端库未按 STMT_ATTR_UPDATE_MAX_LENGTH 更新长度才会发生），绝不把截断的
         *          数据交给调用方。
         * @param statement 已 mysql_stmt_execute + mysql_stmt_store_result 成功的语句句柄
         * @return std::unique_ptr<DatabaseResult> 结果集快照；失败返回 nullptr，原因见 lastError()
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> materializePreparedResult(MYSQL_STMT *statement);

        MYSQL *m_mysqlHandle{nullptr}; ///< MySQL C API 连接句柄，本对象独占所有权，未连接时为 nullptr
        /// 本类开着的事务（beginTransaction 置位，commit/rollback 与连接生命周期重置清零）：
        /// 归还路径据此决定要不要滚，见 resetSessionState 的记账范围说明
        bool m_isTransactionOpen{false};
    };

} // namespace AsynGyanis::Database
