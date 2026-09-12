/**
 * @file PostgresConnection.h
 * @brief PostgreSQL 数据库连接实现
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Database/Common/DatabaseConnection.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// libpq 的全局 C 类型前置声明集中写在本头的全局作用域（全项目只此一处）：
// 只有 .cpp 才包含 <libpq-fe.h>，避免第三方 C 头顺着包含链传染给所有使用方。
// PostgresResult.h 通过包含本头复用下面这几行声明，不得重复声明。
//
// 声明形式必须与 <libpq-fe.h> 完全一致：该头用的是
//   typedef struct pg_conn PGconn;   typedef struct pg_result PGresult;
// 即「标签名 + 同名 typedef」。这里的前置声明（struct 标签 + 同类型别名）与它逐字等价，
// C++ 允许在同一作用域内把同一个 typedef 名重复声明成同一类型，因此先后包含互不冲突。
// 若照 MySQL 那种旧式写法凭空造一个别的标签名（如 struct PGconn），.cpp 里就会得到两个
// 指向不同类型、却能互相隐式转换的句柄，真正的 PQ* 调用全部匹配不上（C2664）。
struct pg_conn;
struct pg_result;

using PGconn   = struct pg_conn;   ///< libpq 连接句柄，对应 <libpq-fe.h> 的 typedef struct pg_conn PGconn;
using PGresult = struct pg_result; ///< libpq 结果集句柄，对应 <libpq-fe.h> 的 typedef struct pg_result PGresult;

namespace AsynGyanis::Database
{
    /**
     * @brief PostgreSQL 数据库连接
     *
     * @details 封装 libpq（PostgreSQL 官方 C 客户端）实现 DatabaseConnection 抽象接口。
     *          本驱动是可选编译的：CMake 找到 libpq 时定义 DATABASE_HAS_POSTGRES 编出真实实现，
     *          否则退化成一个明确报错的桩——connect() 恒为 false，并把「当前构建未编译
     *          PostgreSQL 驱动」写进 lastError()，调用方不会把「什么都没做」误当成成功。
     *
     * ConnectionConfig 的五个字段在本驱动全部有效，且以 **PQconnectdbParams 的关键字/取值对**
     * 下发（不拼连接字符串：拼接要处理引号与转义，参数化接口没有这个问题）：
     * - host / port：连接地址。host 为空视为配置错误当场失败；port 为 0 时**省略**该关键字，
     *   由 libpq 回落到默认端口 5432；
     * - userName / password / database：认证信息与默认库名，按字节原样交给 libpq，本类不做转义。
     *   空串一律省略对应关键字，让 libpq 用它自己的默认规则（用户名默认取操作系统账号、
     *   口令走 PGPASSWORD 或 ~/.pgpass、库名默认取用户名）。传空串与省略在 libpq 里是两种语义：
     *   前者会被当成「值就是空」，服务端会回 "database \"\" does not exist" 一类错误。
     *
     * 超时策略：基类的 connectTimeout() 单位是毫秒，而 libpq 的 connect_timeout 接受的单位是秒，
     *          因此 connect() 向上取整换算后下发（不足 1 秒按 1 秒，避免 500 毫秒被截成 0——
     *          0 在 libpq 里的含义恰好相反：无限等待）。非正值代表「不超时」，此时直接省略该关键字。
     *          该参数只在建连期间生效，没有运行期修改的公开手段，因此 connect() 之后再调用
     *          setConnectTimeout() 不会影响当前会话，必须 disconnect() + connect() 才生效。
     *
     * 字符集：建连成功后立即 PQsetClientEncoding(conn, "UTF8")，保证中文与错误消息都按 UTF-8
     *        往返。libpq 默认跟随服务端/操作系统的编码，在把 client_encoding 设为 SQL_ASCII 或
     *        WIN1252 的服务端上会让中文列变乱码，因此这里显式协商；协商失败即判整次连接失败。
     *
     * 占位符：本驱动直接使用 $1 $2 这种 PostgreSQL 原生写法（PostgresDialect::placeholder() 生成），
     *        由 SQL 文本原样交给 PQexecParams，因此**不做任何 "?" → "$n" 的改写**；
     *        参数个数就是 parameters.size()，与语句里的 $n 个数不一致时由服务端报错，
     *        本驱动只负责把服务端原文转成中文原因。参数格式一律文本（paramFormats 传空指针），
     *        避免二进制格式下每个类型都要自己编码。
     *
     * 命令边界：一次 execute() 只发一条语句（PQexec / PQexecParams 单命令接口）；
     *          服务端支持多语句时也会一次执行全部，但返回值只反映最后一条，本驱动不做多语句编排。
     *
     * 结果集：PostgresResult 在构造时把整份 PGresult 读进内存快照（行、列名、影响行数），
     *        因此结果集不引用连接，可以比连接对象活得更久；代价是大结果集会等额占用内存。
     *        写语句交出的空回执携带语句级影响行数（affectedRowCount()），只读结果集按约定返回 0。
     *
     * 生命周期：构造（不分配句柄、不做 IO）→ connect() → execute() / 事务 → disconnect() → 析构。
     *          析构自动调用 disconnect()。本对象独占 PGconn 句柄，拷贝或移动后的源对象析构时会
     *          重复 PQfinish，因此一律禁止。句柄本身不是线程安全的：一条连接同一时刻只能由
     *          一个线程使用，需要并发就每线程一条连接。
     *
     * 错误处理：所有错误原因写入基类的 m_lastError，lastError() 沿用基类实现，本类不做重复覆写；
     *          面向使用者的文本一律中文，并带上 libpq 原文与可用的 SQLSTATE。
     *          链路被服务端单方面断开（PQstatus 变为 CONNECTION_BAD）时，execute() 的失败路径
     *          会顺手断开连接，调用方重连即可。isConnected() 会读一次 PQstatus（纯本地状态位，
     *          不做网络往返），因此能发现对端已经关掉的连接。
     *
     * @code
     *   auto connection = DatabaseFactory::createPostgres(ConnectionConfig::postgresDefault());
     *   if (connection->connect())
     *   {
     *       auto result = connection->execute("SELECT id, name FROM users WHERE id = $1",
     *                                         std::vector<DatabaseValue>{std::int64_t{1}});
     *       while (result != nullptr && result->next())
     *       {
     *           const DatabaseValue name = result->getValue("name");
     *       }
     *   }
     * @endcode
     */
    class PostgresConnection : public DatabaseConnection
    {
    public:
        /**
         * @brief 使用配置构造 PostgreSQL 连接，此阶段不分配句柄也不发生任何网络交互
         * @param configuration 连接配置；host/port 用于建连，userName/password 用于认证，
         *                      database 为登录后默认选中的库
         */
        explicit PostgresConnection(const ConnectionConfig &configuration);

        /**
         * @brief 析构时自动断开连接，释放底层 PGconn 句柄
         */
        ~PostgresConnection() override;

        // 句柄所有权唯一：拷贝会让两个对象 PQfinish 同一个 PGconn；
        // 移动会让源对象析构时再次 PQfinish（句柄已交给目标对象），
        // 因此拷贝与移动一律禁止（基类同样已删除，这里显式写清意图）。
        PostgresConnection(const PostgresConnection &)            = delete;
        PostgresConnection &operator=(const PostgresConnection &) = delete;
        PostgresConnection(PostgresConnection &&)                 = delete;
        PostgresConnection &operator=(PostgresConnection &&)      = delete;

        /**
         * @brief 连接 PostgreSQL 服务
         * @details 重写 DatabaseConnection::connect()：与基类契约的差异与附加行为——
         *          1) 已连接时直接返回 true，保持幂等（在同一句柄上再次握手会丢掉事务、
         *             会话变量等全部会话状态）；
         *          2) PGconn 句柄在本方法内部才由 PQconnectdbParams 创建，构造阶段不留任何资源；
         *          3) 建连参数以关键字/取值对下发，空值项直接省略（见类注释里空串与省略的差异）；
         *          4) connect_timeout 每次建连都重新读取 connectTimeout() 并做毫秒→秒换算，
         *             因此重连前修改设置即可生效；
         *          5) host 为空视为配置错误，当场失败而不交给 libpq 报出难懂的底层错误；
         *          6) 建连成功后立即协商 UTF8 客户端编码，协商失败同样判整次连接失败；
         *          7) 任何一步失败都先摘好错误文本再 PQfinish，绝不留下半开的连接，
         *             也绝不读悬垂指针（PQerrorMessage 的缓冲随 PQfinish 一起释放）。
         *          桩构建（未编译 libpq）下本方法直接返回 false，并在 lastError() 给出缺失驱动的提示。
         * @return true 连接已建立且客户端编码已协商为 UTF8
         * @return false 任一环节失败，原因（含 libpq 原文）见 lastError()
         * @note 重连前修改超时设置即可生效；已连接状态下修改设置要等下一次 disconnect() + connect()
         */
        bool connect() override;

        /**
         * @brief 断开连接并释放底层 PGconn 句柄
         * @details 重写 DatabaseConnection::disconnect()：与基类的差异——
         *          1) 只负责关句柄与复位状态，不清 m_lastError：调用方常在失败路径上先写好原因再断开，
         *             覆盖它就会把真正的根因丢掉；
         *          2) 已交出的 PostgresResult 自带全部数据（构造时已读进内存快照），断开后仍可正常读取；
         *          3) PQfinish 之后 PQerrorMessage / PQstatus 的返回值即失效，所以本方法内部不再读取
         *             任何错误文本，需要错误信息的失败路径必须「先取文本、再断开」。
         *          未连接且无残留句柄时是安全的空操作，析构函数会无条件调用本方法。
         * @note 建连失败时同样必须调用 PQfinish 回收失败会话的内存，因此 connect() 的失败路径
         *       也走本方法，不需要额外分支
         */
        void disconnect() override;

        /**
         * @brief 判断连接是否可用
         * @details 重写 DatabaseConnection::isConnected()：在基类的 m_isConnected 标志之外
         *          再读一次 PQstatus——它是 libpq 在本地维护的状态位（不发任何网络往返），
         *          对端已经关闭连接或链路已断时它会变成 CONNECTION_BAD，因此本方法能发现
         *          服务端单方面关掉的连接，而不像仅看标志位那样一直报「已连接」。
         *          代价是一次极廉价的本地判断，不引入往返开销。
         * @return true 已连接且 PQstatus 为 CONNECTION_OK
         */
        [[nodiscard]] bool isConnected() const override;

        // 引入基类的全部 execute 重载：本类声明了名为 execute 的成员，按 C++ 名字查找规则
        // 会隐藏基类的同名重载，加上这一行后通过具体对象也能调用全部版本
        using DatabaseConnection::execute;

        /**
         * @brief 执行一条不带参数的 SQL 命令
         * @details 重写 DatabaseConnection::execute()：与基类约定的差异与附加约束——
         *          - 每次调用开头清空 m_lastError，成功调用不会残留上一轮的失败文本；
         *          - 命令文本先拷成 std::string 再把 c_str() 交给 PQexec：libpq 的这个接口
         *            只接受零终止 C 字符串，而 std::string_view 不保证末尾有 '\0'（它可能是
         *            另一个更长缓冲的切片），直接交出 data() 会读到越界内存；
         *          - 只有 PGRES_TUPLES_OK（查询）与 PGRES_COMMAND_OK（写语句/DDL/事务语句）
         *            算成功，其余状态（含 PGRES_FATAL_ERROR、PGRES_COPY_*）一律失败；
         *          - 失败原因优先取 PQresultErrorMessage（挂在本条结果上），为空时退回
         *            PQerrorMessage（连接级错误），保证一定有可读原因；
         *          - PQstatus 变成 CONNECTION_BAD 时顺手断开连接，因为该句柄已无法复用；
         *          - 命令为空时直接失败，不发送任何字节。
         * @param command SQL 文本，例如 "SELECT id, name FROM users"
         * @return std::unique_ptr<DatabaseResult> 结果集；失败返回 nullptr，原因见 lastError()
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> execute(std::string_view command) override;

        /**
         * @brief 执行一条带 $n 占位符的 SQL 命令，参数按位置绑定
         * @details 重写 DatabaseConnection::execute()：与基类默认实现（直接报「暂不支持」）不同，
         *          本驱动用 libpq 的**扩展查询协议**（PQexecParams）真正绑定参数，取值绝不拼进
         *          SQL 文本，注入面因此彻底消失。语句文本**原样送达**：PostgresDialect 生成的
         *          占位符就是 $1 $2 这种 PostgreSQL 原生写法，也就是 PQexecParams 要求的格式，
         *          所以本方法不做任何 "?" → "$n" 的改写；parameters[i] 按下标绑定到 $i+1，
         *          个数就是 parameters.size()，与语句里的 $n 个数不一致时由服务端报错。
         *
         * 执行细节：
         *          1) 参数一律以**文本格式**送出（paramFormats 传空指针，resultFormat 传 0），
         *             不引入二进制编码这一层；值由 Detail::toPostgresTextParameter 转换，
         *             SQL NULL 用空指针表达（绑成空串会让 IS NULL 不再成立）；
         *          2) paramValues 与 paramLengths 成对交出：长度数组非空时 libpq 按长度取字节，
         *             不依赖零终止符，因此内嵌 '\0' 的文本与空串都能被精确表达；
         *          3) 容器类型（List / Hash）被明确拒绝并给出中文原因：正确用法是展开成多个
         *             标量参数（IN 列表由方言展开），静默绑成 NULL 会让调用方以为条件生效了；
         *          4) 本地校验（参数能否绑定、命令是否为空）先于连接状态判定：这两条与连接无关，
         *             越早给出越省一次往返，也让「容器参数被拒」这条判定不需要服务端即可验证。
         *             这是与 MySqlConnection 判定顺序（先连接、后校验参数）的有意差异；
         *          5) 失败路径与不带参数的版本一致：状态非 TUPLES_OK / COMMAND_OK 即失败，
         *             原因取 PQresultErrorMessage（退回 PQerrorMessage），链路已断则顺手断开。
         *
         * @param command 带 $n 占位符的 SQL 文本（由 PostgresDialect 生成），
         *                例如 "SELECT id FROM users WHERE age >= $1"
         * @param parameters 按占位符出现顺序排列的绑定参数，parameters[i] 绑定到 $i+1
         * @return std::unique_ptr<DatabaseResult> 结果集；失败返回 nullptr，原因见 lastError()
         * @note 未连接、参数个数不匹配（由服务端报错）、命令为空、容器类型参数这几条判定
         *       都不依赖具体数据，因此无需服务端即可验证
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> execute(std::string_view command,
                                                              std::span<const DatabaseValue> parameters) override;

        /**
         * @brief 获取数据库类型
         * @details 重写 DatabaseConnection::databaseType()：恒定返回 DatabaseType::PostgreSql，
         *          不依赖连接状态，桩构建下同样返回本类型。
         * @return DatabaseType DatabaseType::PostgreSql
         */
        [[nodiscard]] DatabaseType databaseType() const override;

        /**
         * @brief 开始一个事务
         * @details 执行本方言（PostgresDialect）给出的开启语句 "BEGIN"，是 PostgreSQL 专有能力的
         *          便捷封装，不覆盖基类任何虚函数。语句文本取自方言而非硬编码：
         *          同一件事（开启事务）只能有一份语句来源，否则会随方言演进而漂移，
         *          与 Transaction + 方言这条路径产生行为差异。
         * @return true 事务已开启
         * @return false 未连接或语句被服务端拒绝（例如同一连接上已有未结束的事务），原因见 lastError()
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
         * @details 优先走 PQparameterStatus(conn, "server_version")：这是服务端在握手阶段
         *          主动送来的 GUC 取值（不产生网络往返），形如 "17.11 (Debian 17.11-1.pgdg120+1)"。
         *          本方法只取第一个空格之前的版本号本身（"17.11"）：括号里是发行版的打包信息，
         *          与「服务端版本」无关，裁掉后调用方拿到的文本可以直接参与比较或打印。
         *          取不到时退回 PQserverVersion 给出的整数（形如 170011）按 主版本.次版本 拼装，
         *          与 PostgreSQL 自己的编号规则一致（major = value / 10000，minor = value / 100 % 100）。
         *          未连接时返回空串，与 MySqlConnection::serverVersion() 保持一致口径
         *          （服务端版本必须持有已建立连接的句柄才有值，这与 SQLite 那种进程内引擎不同）。
         * @return std::string 形如 "17.11" 的服务端版本号；未连接、桩构建或取值失败时为空串
         */
        [[nodiscard]] std::string serverVersion() const;

        /**
         * @brief 获取底层 PGconn 句柄，供需要直接使用 libpq 的高级场景使用
         * @warning 句柄所有权始终属于本连接：调用方不得 PQfinish，也不得在连接销毁后继续使用；
         *          拿它去发命令会绕过本类的错误处理、编码协商与断链复位逻辑
         * @return PGconn* 未连接（或桩构建）时为 nullptr
         */
        [[nodiscard]] PGconn *nativeHandle() const noexcept { return m_connection; }

    private:
        /**
         * @brief 采集连接级错误文本并写入 m_lastError
         * @param description 面向使用者的中文动作说明，例如「连接失败」
         */
        void captureError(std::string_view description);

        /**
         * @brief 采集结果集上的错误文本与 SQLSTATE 并写入 m_lastError
         * @details 结果集错误挂在 PGresult 上而不是连接上，必须用 PQresultErrorMessage 取；
         *          该接口对「命令本身成功但状态不对」的情形可能给出空串，此时退回连接级的
         *          PQerrorMessage，保证 m_lastError 永远有可读原因。
         * @param result 出错的结果集句柄，调用方保证非空
         * @param description 面向使用者的中文动作说明，例如「执行 SQL 命令失败」
         */
        void captureResultError(PGresult *result, std::string_view description);

        /**
         * @brief 校验 libpq 的结果状态并把它转成 DatabaseResult
         * @details 两个 execute 重载的收尾逻辑完全一致，收敛到一处可以避免「一条路径忘了回收句柄、
         *          另一条忘了断开坏连接」这类不对称缺陷：空指针与状态不在白名单内的都算失败，
         *          失败时先摘错误文本（PQresultErrorMessage 退回 PQerrorMessage）再 PQclear，
         *          最后读一次 PQstatus，链路已断就顺手断开本连接。
         * @param rawResult libpq 交出的结果集句柄，可为空指针（libpq 内存不足时即如此）
         * @param description 失败时面向使用者的中文动作说明，例如「执行 SQL 命令失败」
         * @return std::unique_ptr<DatabaseResult> 结果集；失败返回 nullptr，原因见 lastError()
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> takeResult(PGresult *rawResult, std::string_view description);

        PGconn *m_connection{nullptr}; ///< libpq 连接句柄，本对象独占所有权，未连接时为 nullptr
    };

} // namespace AsynGyanis::Database
