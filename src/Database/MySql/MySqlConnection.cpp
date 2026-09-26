// windows.h 会把 min / max 定义成函数式宏（libmysqlclient 的头会间接包含它），
// 让 std::numeric_limits<T>::max() 与 std::max/std::min 一律编译不过（C4003/C2589）。
// 必须在任何头文件之前定义 NOMINMAX 才能挡住这对宏：放在文件最顶部是唯一与包含顺序无关的写法。
// 它只影响本翻译单元，本文件全程使用 std:: 限定写法，不依赖这两个宏
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Database/MySql/MySqlConnection.h"

#include "Database/Common/BinaryBytes.h"
#include "Database/Common/ErrorText.h"
#include "Database/Dialect/MySqlDialect.h"
#include "Database/MySql/MySqlResult.h"
#include "Database/MySql/MySqlStatementResult.h"

#ifdef DATABASE_HAS_MYSQL

// libmysqlclient / MariaDB Connector/C 的头文件只在本实现文件里包含，对外只暴露 MySqlConnection.h 的前置声明。
// 这两个头自带平台网络头的包含顺序，调用方无需先包含 winsock2.h 之类。
//
// 包含路径有「扁平」与「带 mysql/ 子目录」两种发行布局，这里用 __has_include 同时兼容而不是靠平台宏：
// - 多数 Linux 发行版与官方源码包把 mysql.h / errmsg.h 直接放在顶层（<mysql.h>），
//   而 <mysql/mysql.h> 是 Debian 系私有头目录（libmariadb-dev 之类的兼容路径）；
// - Conan 上的 Windows 包是扁平布局：include/ 下直接是 mysql.h，include/mysql/ 里只有
//   client_plugin.h 等插件头，没有任何入口头。写死任一形式都会在另一种包上找不到文件，
//   而两种布局下都包含正确的头只多一次预处理探测，没有运行期代价
#if __has_include(<mysql/mysql.h>)
#include <mysql/errmsg.h>
#include <mysql/mysql.h>
#else
// CR_SERVER_GONE_ERROR / CR_SERVER_LOST 等连接级错误码定义在 errmsg.h 里
#include <errmsg.h>
#include <mysql.h>
#endif

// 列值到 DatabaseValue 的类型映射与文本协议路径共用一份实现，保证两条协议路径取值语义一致。
// 该头必须看到真实的 mysql.h（要取 enum_field_types 常量），因此只能放在本分支内
#include "Database/MySql/MySqlValueConversion.h"

#include <limits>

#endif // DATABASE_HAS_MYSQL

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
#ifdef DATABASE_HAS_MYSQL

    namespace
    {
        // 用 constexpr 常量取代宏：单位换算与协议上限的出处集中在此，类型安全且作用域受控
        constexpr int kMillisecondsPerSecond = 1000; ///< 1 秒等于 1000 毫秒，毫秒→秒换算的唯一依据

        // 连接字符集：utf8mb4 才能真正存下 emoji 与全部 BMP 之外的汉字（utf8mb3 不行）
        constexpr const char *kConnectionCharacterSet = "utf8mb4";

        // 命令文本与参数绑定的长度都交给 unsigned long 形参（Windows 上是 32 位），
        // 超限会被静默截断成半条语句或半段数据，因此共用同一上限并在交给客户端库前拦下
        constexpr size_t kMaximumNativeLength = static_cast<size_t>(std::numeric_limits<unsigned long>::max());

        // 零长度二进制载荷要交给 MYSQL_BIND 一个合法指针：buffer 为空指针时客户端库的行为
        // 没有文档保证，因此指向这个静态字节，长度仍按 0 填写
        constexpr std::uint8_t kEmptyBinaryPayloadByte = 0;

        /**
         * @brief 把客户端库给出的影响行数转成 ORM 口径，出错哨兵归零
         * @param rawCount mysql_affected_rows / mysql_stmt_affected_rows 的原始返回值
         * @return std::int64_t 影响行数；客户端库出错时返回 0（「未知」）
         */
        std::int64_t toAffectedRowCount(const my_ulonglong rawCount) noexcept
        {
            // 出错时这两个接口返回 (my_ulonglong)-1，直接转换会让调用方看到 -1 行
            return rawCount == static_cast<my_ulonglong>(-1) ? 0 : static_cast<std::int64_t>(rawCount);
        }

        /**
         * @brief 把毫秒超时换算成 MySQL 客户端选项需要的整秒
         * @param milliseconds 时长毫秒数，来自基类的 connectTimeout() / queryTimeout()
         * @return unsigned int 秒数；非正值折算为 0，含义是「不超时」
         */
        unsigned int toClientSeconds(const int milliseconds)
        {
            // 非正值不走换算：0 在客户端库里就是「不超时」的约定值，负数没有意义，一并归到该语义
            if (milliseconds <= 0)
            {
                return 0U;
            }

            // 单位换算取向上取整：客户端选项只吃整秒，若向下取整，500 毫秒会被截成 0——
            // 而 0 的语义恰好相反（永不超时），一个「更短的超时」变成了「没有超时」
            const auto positiveMilliseconds = static_cast<unsigned int>(milliseconds);
            return (positiveMilliseconds + static_cast<unsigned int>(kMillisecondsPerSecond) - 1U) / static_cast<unsigned int>(kMillisecondsPerSecond);
        }

        /**
         * @brief MYSQL_RES 的自定义释放器，用来罩住「已 store_result、尚未交给结果集」这段所有权真空
         */
        struct ResultReleaser
        {
            /**
             * @brief 释放结果集句柄
             * @param ownedResult 待释放的 MYSQL_RES，可为空指针
             */
            void operator()(MYSQL_RES *ownedResult) const noexcept
            {
                // 判空后再释放：早期客户端版本不保证 mysql_free_result(nullptr) 是安全的空操作
                if (ownedResult != nullptr)
                {
                    mysql_free_result(ownedResult);
                }
            }
        };

        /**
         * @brief 把只读数据的地址交给 MySQL C API 要求的 void* 形参
         * @details 绑定参数时缓冲区的内容只被客户端库读取（在 mysql_stmt_execute 内部写进网络包），
         *          但 C API 的形参类型是非 const 的 void*，因此这里必须去掉 const 限定。
         *          去掉 const 后本函数也不写入任何字节，调用方更不得借该指针修改原数据。
         * @param valueAddress 待绑定数据的地址
         * @return void* 同一个地址的非 const 形式
         */
        void *asBindBuffer(const void *const valueAddress) noexcept
        {
            return const_cast<void *>(valueAddress);
        }

        /// 取值缓冲区的下限：max_length 为 0 的列（整列都是 NULL 或空串）也必须拿到一个合法指针
        constexpr unsigned long kMinimumColumnBufferBytes = 1UL;
    } // namespace

    MySqlConnection::MySqlConnection(const ConnectionConfig &configuration)
    {
        // 基类的 m_configuration 是唯一真值来源；构造阶段既不 mysql_init 也不下发选项，
        // 否则「构造一个连接对象」这件事就带上了失败语义，超时设置也会被冻结在构造那一刻
        m_configuration = configuration;
    }

    bool MySqlConnection::connect()
    {
        // 已连接时直接返回，满足基类对 connect() 幂等的要求：
        // 在同一个句柄上再次 mysql_real_connect 相当于隐式重连，会丢掉事务与会话变量等全部会话状态
        if (m_isConnected)
        {
            return true;
        }

        // 清掉上一轮的失败文本，避免成功路径上 lastError() 仍报旧错
        m_lastError.clear();

        // 空主机交给客户端库只会得到一条难懂的底层错误（而且不同版本行为还不一致），这里前置拦一道
        if (m_configuration.host.empty())
        {
            m_lastError = "MySQL 主机地址未配置";
            return false;
        }

        // 句柄在连接阶段才创建。失败路径统一把句柄关掉并置空，所以走到这里的空句柄一定是全新的；
        // 若上一次是 disconnect() 之后重连，也正好按最新的超时设置重新 mysql_init + 下发选项
        if (m_mysqlHandle == nullptr)
        {
            m_mysqlHandle = mysql_init(nullptr);
            if (m_mysqlHandle == nullptr)
            {
                m_lastError = "创建 MySQL 连接句柄失败：mysql_init 返回空指针（内存分配不足）";
                return false;
            }
        }

        // 超时与字符集必须在握手之前下发：客户端库只在 mysql_real_connect 内部读取它们一次
        if (!applyConnectionOptions())
        {
            // 原因已由 applyConnectionOptions 写进 m_lastError；disconnect() 刻意不动它，
            // 这里只负责释放句柄，不留半个无法复用的对象
            disconnect();
            return false;
        }

        // database 为空串时不选择默认库，这是 mysql_real_connect 的正规用法之一；
        // mysql_real_connect 只接受零终止 C 字符串：配置里内嵌 '\0' 会让它静默截断成半截，
        // 表现成「认证失败」这类与真实原因无关的报错，因此先本地拦下
        const std::string_view configurationTexts[] = {m_configuration.host, m_configuration.userName, m_configuration.password, m_configuration.database};
        for (const std::string_view configurationText: configurationTexts)
        {
            if (configurationText.find('\0') != std::string_view::npos)
            {
                m_lastError = "连接 MySQL 服务失败：连接配置（host / userName / password / database）含内嵌 NUL 字节，"
                              "客户端库只接受零终止字符串并会静默截断——请检查配置的来源";
                return false;
            }
        }

        // CLIENT_FOUND_ROWS 让 UPDATE/DELETE 的回执报「WHERE 匹配到多少行」而不是「实际改了几行的值」。
        // 默认那套（改值才算）是 MySQL 独有的：同一条 ORM 更新在 SQLite 上报 1、在这里报 0，而调用方
        // 拿这个数判的就是「那行在不在」——把值改回原样并不能让行凭空消失。开这个位把两侧对齐。
        // 其余位照旧不开：CLIENT_MULTI_STATEMENTS（一次一条语句，别给拼接注入留门）与 LOCAL_INFILE。
        if (mysql_real_connect(m_mysqlHandle, m_configuration.host.c_str(), m_configuration.userName.c_str(), m_configuration.password.c_str(), m_configuration.database.c_str(),
                               m_configuration.port, nullptr, CLIENT_FOUND_ROWS) == nullptr)
        {
            // 先摘 mysql_error 的文本再 disconnect：mysql_close 会释放错误缓冲，顺序反了就读到悬垂指针
            captureError("连接 MySQL 服务失败");
            disconnect();
            return false;
        }

        // 全部步骤走通才置位：中途任何一步失败都不会让 isConnected() 读到「已连接」的中间态。
        // 事务标记随新会话一起归零：新连接上不可能开着上一次连接的事务
        m_isConnected       = true;
        m_isTransactionOpen = false;

        // 只读语句的服务端时限只能在握手之后下发（它是一条普通语句）。设不上不该把一次成功的连接判成失败：
        // 没有这道界时客户端读写超时仍在守着，只是那一道会连连接一起废掉，原因由 lastError() 给出。
        // 但若这条语句把连接本身弄断了（握手刚完就失联），那就不是一条可用的会话，如实判失败
        if (!applyStatementTimeLimit() && !isConnected())
        {
            return false;
        }

        return true;
    }

    void MySqlConnection::disconnect()
    {
        // 既没连过也没有残留句柄时是安全的空操作，析构函数会无条件调用本方法
        if (!m_isConnected && m_mysqlHandle == nullptr)
        {
            return;
        }

        // 先落状态再关句柄：关闭过程中若有回调读 isConnected()，也应看到「已断开」。
        // 这里不清 m_lastError——失败路径上先写好的原因不能被断开动作抹掉。
        // 事务标记一并归零：连接都没了，谈不上还开着事务（服务端那边断开即回滚）
        m_isConnected       = false;
        m_isTransactionOpen = false;

        if (m_mysqlHandle == nullptr)
        {
            return;
        }

        // 缓存的语句属于**当前这个**连接句柄：mysql_close 不会替我们关闭它们，留着就是服务端语句
        // 与客户端内存双双泄漏；而重连之后拿旧语句去 execute 更是对已释放对象的访问。
        // 顺序必须是「先关语句、再关连接」，与 SQLite 驱动那条缓存纪律同一依据
        clearStatementCache();

        // mysql_close 释放句柄内部的全部缓冲，其中就包括 mysql_error() 指向的那一份，
        // 因此所有错误文本都必须在本行之前取走（connect()/execute() 的失败路径都遵守这一顺序）。
        // 已由 mysql_store_result 预读出去的 MySqlResult 用的是自己的内存，不受本步影响
        mysql_close(m_mysqlHandle);
        m_mysqlHandle = nullptr;
    }

    bool MySqlConnection::isConnected() const
    {
        // 双判据：m_isConnected 是逻辑状态，句柄非空才是物理事实；两者不一致说明有路径漏置位，
        // 一律按未连接处理更安全。这里刻意不发 mysql_ping：一次往返的代价对高频查询不可接受，
        // 链路失活由 execute() 的失败路径发现并顺带断开
        return m_isConnected && m_mysqlHandle != nullptr;
    }

    std::unique_ptr<DatabaseResult> MySqlConnection::execute(const std::string_view command)
    {
        // 每次调用都是独立尝试：先清空错误，成功调用不会残留上一轮的失败文本
        m_lastError.clear();

        if (!isConnected())
        {
            m_lastError = "未连接到 MySQL 数据库，命令未执行";
            return nullptr;
        }

        if (command.empty())
        {
            m_lastError = "数据库命令为空";
            return nullptr;
        }

        // mysql_real_query 的长度形参是 unsigned long：Windows(LLP64) 上它是 32 位，超过上限的命令
        // 会被静默截断成半条语句，宁可报错也不执行残缺命令。Linux/macOS(LP64) 上两者等宽，
        // 这条比较恒假（会招来 -Wtype-limits 告警），因此只在确实存在窄化风险的平台上判
        if constexpr (sizeof(std::string_view::size_type) > sizeof(unsigned long))
        {
            if (command.size() > kMaximumNativeLength)
            {
                m_lastError = "数据库命令过长：" + std::to_string(command.size()) + " 字节，超出 MySQL 客户端协议上限";
                return nullptr;
            }
        }

        // mysql_real_query 按「指针 + 长度」取参，本身二进制安全，因此可以直接交出 string_view 的
        // data()（不像 sqlite3_prepare_v2 那样依赖零终止符）。超长命令已在上面判掉，这里的窄化是安全的
        if (mysql_real_query(m_mysqlHandle, command.data(), static_cast<unsigned long>(command.size())) != 0)
        {
            // 错误码要在摘文本之前抓：两者都指向句柄里的同一份状态，顺序不影响取值，但都必须早于 disconnect
            const unsigned int errorNumber = mysql_errno(m_mysqlHandle);
            captureError("执行 SQL 命令失败");

            // CR_SERVER_GONE_ERROR / CR_SERVER_LOST 代表链路已断，这个句柄再也无法复用；
            // 顺手断开并释放，让 isConnected() 与后续 execute() 的判定保持一致，调用方重连即可
            if (errorNumber == CR_SERVER_GONE_ERROR || errorNumber == CR_SERVER_LOST)
            {
                disconnect();
            }

            return nullptr;
        }

        // store_result 把整份结果（行数据 + 列元数据）一次性复制进客户端内存：
        // 之后结果集与连接再无关系，可以比连接活得更久，遍历过程中也不会再有任何网络往返。
        // 代价是大结果集等额占内存；需要流式读取的场景应改用 mysql_use_result，本驱动不提供
        MYSQL_RES *rawResult = mysql_store_result(m_mysqlHandle);
        if (rawResult == nullptr)
        {
            // mysql_field_count() == 0 表示这条命令本就没有返回列（INSERT/UPDATE/DELETE/DDL/事务语句），
            // 属于「执行成功的空回执」，不是错误：交出一个 0 行 0 列的结果集，让调用方只需判 nullptr。
            // 影响行数必须在这里就地取：mysql_affected_rows 给的是「最近一条命令」的语句级计数，
            // 下一条命令一执行就被覆盖，因此不能拖到调用方读取时再取。
            // 只在写语句分支取，是因为查询下它返回的是「返回了多少行」，冒充影响行数会误导调用方
            // 自增标识与它同一条纪律、同一个分支：文本协议下的 mysql_insert_id 同样是语句级值
            if (mysql_field_count(m_mysqlHandle) == 0)
            {
                return std::make_unique<MySqlResult>(nullptr, toAffectedRowCount(mysql_affected_rows(m_mysqlHandle)), mysql_insert_id(m_mysqlHandle));
            }

            // 有返回列却没拿到结果集。这里要分两种，判据是客户端库文档给的：mysql_store_result 返回 NULL
            // 且 mysql_errno() 非 0，说明是「服务端报错」（3024 语句超时、递归深度超限这类），ERR 包已被
            // 本次调用消化完，链路完好，连接必须留着继续用；只有 mysql_errno() 为 0 才是客户端预读时
            // 分配不出内存——那时回复流的位置不可知，这条连接才真的不能再用于发命令。
            // 早先这里不分情形一律断开，于是任何一句被服务端拒绝的查询都会白废一条池里的连接
            if (const unsigned int storeError = mysql_errno(m_mysqlHandle); storeError != 0)
            {
                captureError("执行 SQL 命令失败");
                return nullptr;
            }

            // 依旧是先摘文本、再断开
            captureError("读取 MySQL 结果集失败");
            disconnect();
            return nullptr;
        }

        // 用带自定义释放器的 unique_ptr 罩住这段所有权真空：make_unique 若因内存分配失败抛异常，
        // MYSQL_RES 会由守卫释放，不会泄漏
        std::unique_ptr<MYSQL_RES, ResultReleaser> guardedResult{rawResult};
        auto                                       result = std::make_unique<MySqlResult>(guardedResult.get());

        // 构造成功，所有权正式移交结果集：此后再由 MySqlResult 的析构负责 mysql_free_result
        guardedResult.release();
        return result;
    }

    std::unique_ptr<DatabaseResult> MySqlConnection::execute(const std::string_view command, const std::span<const DatabaseValue> parameters)
    {
        // 每次调用都是独立尝试：先清空错误，成功调用不会残留上一轮的失败文本
        m_lastError.clear();

        // 未连接时绝不触碰 mysql_stmt_init：空句柄会得到含义不明的底层错误
        if (!isConnected())
        {
            m_lastError = "未连接到 MySQL 数据库，命令未执行";
            return nullptr;
        }

        if (command.empty())
        {
            m_lastError = "数据库命令为空";
            return nullptr;
        }

        // 与不带参数的 execute() 同一条窄化判定：mysql_stmt_prepare 的长度形参也是 unsigned long，
        // 超长命令会被静默截断成半条语句。Linux/macOS(LP64) 上两者等宽，该比较恒假，因此只在有风险时判
        if constexpr (sizeof(std::string_view::size_type) > sizeof(unsigned long))
        {
            if (command.size() > kMaximumNativeLength)
            {
                m_lastError = "数据库命令过长：" + std::to_string(command.size()) + " 字节，超出 MySQL 客户端协议上限";
                return nullptr;
            }
        }

        // 预处理语句按文本复用：mysql_stmt_prepare 是一趟实打实的网络往返（COM_STMT_PREPARE），
        // mysql_stmt_close 又是一趟（COM_STMT_CLOSE）。真机实测同一条主键查询「每次重编」414 µs、
        // 「备好复用」197 µs，省掉的正是这两趟。
        MYSQL_STMT *rawStatement = findCachedStatement(command);
        if (rawStatement == nullptr)
        {
            // 句柄刚创建时尚未入表，本函数负责把它收干净：init 只有内存不足才会返回空，
            // 之后的 attr_set / prepare 失败路径都显式 mysql_stmt_close，不留服务端资源
            rawStatement = mysql_stmt_init(m_mysqlHandle);
            if (rawStatement == nullptr)
            {
                // mysql_stmt_init 只在内存不足时返回空，错误状态仍记在连接句柄上
                captureError("创建 MySQL 预处理语句句柄失败");
                return nullptr;
            }

            // 打开「store_result 时顺带更新每列 max_length」这一属性：取值缓冲区正是按 max_length 分配的，
            // 而实测本客户端库对 BLOB/TEXT 列给出的初始 max_length 是 0（不是类型上限）。少了这个属性，
            // 每行每列都会因长度超出 1 字节的最小缓冲而改走 mysql_stmt_fetch_column 逐列补取——结果仍对，
            // 但每列多搬一次。该属性只影响元数据，且入表后一路保留，只需在首次编译时设一次
            constexpr bool kUpdateMaximumLength = true;
            if (mysql_stmt_attr_set(rawStatement, STMT_ATTR_UPDATE_MAX_LENGTH, &kUpdateMaximumLength) != 0)
            {
                captureStatementError(rawStatement, "设置 MySQL 预处理语句属性失败");
                mysql_stmt_close(rawStatement); // 还没入表，由本函数负责收尾
                return nullptr;
            }

            // 语句文本按「指针 + 长度」交给客户端库，本身二进制安全，不要求零终止
            if (mysql_stmt_prepare(rawStatement, command.data(), static_cast<unsigned long>(command.size())) != 0)
            {
                // 语法错误、表不存在、占位符写法不被支持等都在这一步暴露，错误挂在语句句柄上
                captureStatementError(rawStatement, "预处理 SQL 语句失败");
                mysql_stmt_close(rawStatement); // 同上：这条从未进过表
                return nullptr;
            }

            // 编译成功即由表接管所有权：此后任何失败路径都不得 close 本地这份指针，只能 discard
            cacheStatement(std::string{command}, rawStatement);
        }

        // 绑定与执行必须成对完成：绑定缓冲区是 bindAndExecuteStatement 的局部变量，
        // 只有在该函数内部（mysql_stmt_execute 期间）才是有效的
        if (!bindAndExecuteStatement(rawStatement, parameters))
        {
            // 失败即弃。这条要么真的坏了，要么服务端的表/列已被换掉——留着它下一次仍会失败。
            // 刻意不「重新编译再试一次」：写语句重试就是二次写入，那代价远大于多一趟往返。
            // 断链分支里 disconnect() 已整表清过，这里对同一个键是空操作
            discardCachedStatement(command);
            return nullptr;
        }

        // 无返回列 = 写语句：交出一个 0 行 0 列的写回执，影响行数就地快照
        // （mysql_stmt_affected_rows 给的是语句级计数，下一条命令一执行就被覆盖）
        // 自增标识同理：它来自本次执行收到的 OK 包，跟着一起快照，调用方就不必再发一条查询去问
        if (mysql_stmt_field_count(rawStatement) == 0)
        {
            return std::make_unique<MySqlResult>(nullptr, toAffectedRowCount(mysql_stmt_affected_rows(rawStatement)), mysql_stmt_insert_id(rawStatement));
        }

        // 有返回列 = 查询：先把整份结果从服务端读进客户端内存，之后逐行 fetch 不再有任何网络往返。
        // 与文本协议路径的 mysql_store_result 是同一种取舍：大结果集等额占内存，
        // 换来的是结果集不引用语句句柄，可以比连接活得更久
        if (mysql_stmt_store_result(rawStatement) != 0)
        {
            captureStatementError(rawStatement, "预读 MySQL 结果集失败");
            discardCachedStatement(command);
            return nullptr;
        }

        // 预读成功后把行数据搬进内存快照；结果集与语句就此再无关系
        std::unique_ptr<DatabaseResult> result = materializePreparedResult(rawStatement);
        if (result == nullptr)
        {
            // 快照不完整（元数据缺失、某列取回失败）时结果不能交出去；这条语句同样弃掉
            discardCachedStatement(command);
            return nullptr;
        }

        // 显式释放已读完的结果缓冲：正确性不依赖这一行（实测省掉它结果仍然对），要的是**及时**——
        // 缓存里的语句会长期活着，不释放就把整份结果的客户端内存一直占着。
        // 对 store_result 过的结果它只清本地缓冲，不再产生网络往返；万一失败，快照本身仍然有效
        // （数据已经搬走），只是这条语句不能再复用，弃掉即可，不因此把成功的一次查询改成失败
        if (mysql_stmt_free_result(rawStatement) != 0)
        {
            discardCachedStatement(command);
        }

        return result;
    }

    std::string MySqlConnection::serverVersion() const
    {
        // mysql_get_server_info 必须持有已建立连接的句柄，未连接时没有服务端版本可言
        if (!isConnected())
        {
            return {};
        }

        const char *rawVersion = mysql_get_server_info(m_mysqlHandle);
        return rawVersion != nullptr ? std::string(rawVersion) : std::string{};
    }

    bool MySqlConnection::applyConnectionOptions()
    {
        // 三个超时选项的参数类型都是 unsigned int、单位都是秒，与基类的毫秒语义差一个量纲。
        // 值必须放在本函数持有的局部变量里：mysql_options 在调用点就把值拷进句柄，
        // 因此局部变量随本函数结束析构是安全的
        const unsigned int connectionTimeoutSeconds = toClientSeconds(connectTimeout());
        const unsigned int ioTimeoutSeconds         = toClientSeconds(queryTimeout());

        // 逐条下发，任一选项被拒就整体判失败：宁可连不上也不留一条没有超时保护的会话
        const auto applyOption = [this](const mysql_option option, const void *argumentValue, const std::string_view description) -> bool
        {
            // mysql_options 返回非 0 只可能是「这个版本的客户端库不支持该选项」或参数指针为空，
            // 具体原因同样写在句柄的错误状态里，交给 captureError 摘取
            if (mysql_options(m_mysqlHandle, option, argumentValue) == 0)
            {
                return true;
            }

            captureError(description);
            return false;
        };

        // 连接阶段（TCP 建连 + 握手 + 认证）的等待上限
        if (!applyOption(MYSQL_OPT_CONNECT_TIMEOUT, &connectionTimeoutSeconds, "设置 MySQL 连接超时"))
        {
            return false;
        }

        // 读超时：从服务端读一条回复最多等这么久，服务端卡住时本端不会无限阻塞
        if (!applyOption(MYSQL_OPT_READ_TIMEOUT, &ioTimeoutSeconds, "设置 MySQL 读超时"))
        {
            return false;
        }

        // 写超时：网络半断时发送命令会一直卡在 socket write 上，因此读与写都要设。
        // 读写共用 queryTimeout()：一条命令的预算本就该覆盖「发出去 + 读回来」整个来回
        if (!applyOption(MYSQL_OPT_WRITE_TIMEOUT, &ioTimeoutSeconds, "设置 MySQL 写超时"))
        {
            return false;
        }

        // 字符集在握手期一次定成 utf8mb4：服务端不认识该字符集时这里就会失败，
        // 好过连上之后中文列被按 latin1 解读成乱码
        return applyOption(MYSQL_SET_CHARSET_NAME, kConnectionCharacterSet, "设置 MySQL 连接字符集");
    }

    bool MySqlConnection::applyStatementTimeLimit()
    {
        const int timeoutMilliseconds = queryTimeout();

        // 会话变量的单位与基类一致（毫秒），不做换算；非正值下发 0，含义是服务端不设限——
        // 不下发会把上一次设过的值继续留着，让「改成不设时限」变成一句空话。
        // 这个数来自 queryTimeout() 这个 int，不是外部文本，因此直接拼进语句不引入注入面
        const std::string statementText = "SET SESSION MAX_EXECUTION_TIME = " + std::to_string(timeoutMilliseconds > 0 ? timeoutMilliseconds : 0);

        if (execute(statementText) != nullptr)
        {
            return true;
        }

        // execute() 已把服务端原文写进 lastError（不认识该变量的分支会报 Unknown system variable）。
        // 再补一句这条会话少了哪道界：调用方要能看出「毫秒级语句时限没生效，只剩会连连接一起废掉的那道」
        m_lastError += "；该会话的只读语句时限未能下发，之后只有客户端读写超时兜底（那道会连连接一起断开）";
        return false;
    }

    void MySqlConnection::applyQueryTimeoutNow() noexcept
    {
        // 未连接时无处可发：connect() 会在握手成功后按最新值下发一次
        if (!isConnected())
        {
            return;
        }

        static_cast<void>(applyStatementTimeLimit());
    }

    void MySqlConnection::captureError(const std::string_view description)
    {
        // mysql_error / mysql_errno 只反映句柄上最后一次调用的结果，且返回指针只到下一次
        // 使用该句柄的 API 调用之前有效，必须立刻拷进 std::string；句柄为空时不能调用它们
        if (m_mysqlHandle == nullptr)
        {
            m_lastError = std::string(description) + "：MySQL 连接句柄未初始化";
            return;
        }

        const unsigned int errorNumber = mysql_errno(m_mysqlHandle);
        const char        *rawMessage  = mysql_error(m_mysqlHandle);

        // 带上错误码：只留一句中文 + 服务端英文原文时，排查 1045 / CR_* 之类问题仍需原始数字
        m_lastError = composeNativeErrorText(description, rawMessage != nullptr ? rawMessage : "", "客户端库未给出原因", errorNumber);
    }

    void MySqlConnection::captureStatementError(MYSQL_STMT *const statement, const std::string_view description)
    {
        // 句柄为空时不能调用 mysql_stmt_errno / mysql_stmt_error，只留一句可读原因
        if (statement == nullptr)
        {
            m_lastError = std::string(description) + "：MySQL 预处理语句句柄未初始化";
            return;
        }

        // 预处理语句的错误状态挂在语句句柄上：连接级 mysql_errno 此时读到的可能是上一条
        // 连接操作的陈旧错误，必须用 mysql_stmt_* 这一对接口
        const unsigned int errorNumber = mysql_stmt_errno(statement);
        const char        *rawMessage  = mysql_stmt_error(statement);

        // 文本同样必须先拷贝再让调用方关闭语句：mysql_stmt_close 会释放该缓冲
        m_lastError = composeNativeErrorText(description, rawMessage != nullptr ? rawMessage : "", "客户端库未给出原因", errorNumber);
    }

    MYSQL_STMT *MySqlConnection::findCachedStatement(const std::string_view statementText) noexcept
    {
        // 按视图查：键是 std::string 而形参是 string_view，命中时不必为一次查表再拷一份语句文本
        const auto entry = m_statementCache.find(statementText);
        if (entry == m_statementCache.end())
        {
            return nullptr;
        }

        // 命中即把这条推到最新，并累计一次命中（一次命中省下的是一整趟 COM_STMT_PREPARE 往返）
        entry->second.lastUseStamp = ++m_statementCacheUseStamp;
        ++m_statementCacheHits;
        return entry->second.statement;
    }

    void MySqlConnection::cacheStatement(std::string statementText, MYSQL_STMT *statement) noexcept
    {
        // 命中过的那条本来就在表里，此时表满也不该逐出——那会把一条正被反复使用的语句为一件
        // 本来就不必做的事扔掉，所以先用一次查找把它排除掉；只有真正新增一条键才可能触到上限
        if (m_statementCache.size() >= kMaximumCachedStatements && m_statementCache.find(statementText) == m_statementCache.end())
        {
            evictLeastRecentlyUsedStatement();
        }

        // try_emplace 对已存在的键是空操作：既不会把表里那份换成同一个指针，也不会漏关谁
        static_cast<void>(m_statementCache.try_emplace(std::move(statementText), CachedStatement{statement, ++m_statementCacheUseStamp}));
    }

    void MySqlConnection::evictLeastRecentlyUsedStatement() noexcept
    {
        auto oldest = m_statementCache.begin();
        for (auto candidate = std::next(m_statementCache.begin()); candidate != m_statementCache.end(); ++candidate)
        {
            if (candidate->second.lastUseStamp < oldest->second.lastUseStamp)
            {
                oldest = candidate;
            }
        }

        // 表里的语句都没有未取完的结果（结果已整份物化进快照，或本就是写回执），因此逐出这一条
        // 不会与在用的结果集抢句柄所有权
        mysql_stmt_close(oldest->second.statement);
        m_statementCache.erase(oldest);
    }

    void MySqlConnection::discardCachedStatement(const std::string_view statementText) noexcept
    {
        const auto entry = m_statementCache.find(statementText);
        if (entry == m_statementCache.end())
        {
            // 断链路径上 disconnect() 已经整表关过，这里再关一次就是对已释放句柄的操作
            return;
        }

        mysql_stmt_close(entry->second.statement);
        m_statementCache.erase(entry);
    }

    void MySqlConnection::clearStatementCache() noexcept
    {
        for (auto &[statementText, cachedStatement]: m_statementCache)
        {
            static_cast<void>(statementText);
            mysql_stmt_close(cachedStatement.statement);
        }
        m_statementCache.clear();
    }

    bool MySqlConnection::bindAndExecuteStatement(MYSQL_STMT *const statement, const std::span<const DatabaseValue> parameters)
    {
        // 参数个数必须与占位符个数严格相等：MySQL 对未绑定的占位符按 NULL 参与运算，
        // 少给参数会让条件静默变成永假（WHERE id = NULL），几乎不可能从结果上反推原因，
        // 因此这里宁可当场失败也不做任何「缺省补 NULL」的宽容处理
        if (const auto expectedParameterCount = static_cast<std::size_t>(mysql_stmt_param_count(statement)); expectedParameterCount != parameters.size())
        {
            m_lastError = parameterCountMismatchText(expectedParameterCount, parameters.size());
            return false;
        }

        // 绑定缓冲区必须活到 mysql_stmt_execute 返回为止：客户端库正是在 execute 内部读取它们
        // 并写进网络包。因此这些容器都声明在本函数体内，绑定与执行绝不被拆成两个函数。
        // 值初始化（vector 的默认构造）会把 MYSQL_BIND 的所有字段清零，省去逐个字段赋默认值
        std::vector<MYSQL_BIND>    bindings(parameters.size());
        std::vector<unsigned long> parameterLengths(parameters.size(), 0UL);
        std::vector<signed char>   tinyValues(parameters.size(), 0);

        for (std::size_t index = 0; index < parameters.size(); ++index)
        {
            MYSQL_BIND          &binding        = bindings[index];
            const DatabaseValue &parameterValue = parameters[index];

            // 用 std::get_if 取指针而不是 std::get：类型不符时走到 else 分支给出中文错误，
            // 而 std::get 会抛 std::bad_variant_access，把「参数类型不对」变成难以处理的异常
            if (std::holds_alternative<std::monostate>(parameterValue))
            {
                // SQL NULL 必须用 MYSQL_TYPE_NULL 表达：绑成空字符串后 "IS NULL" 不再成立，
                // 与调用方传空值的意图直接冲突。NULL 不需要任何缓冲区
                binding.buffer_type = MYSQL_TYPE_NULL;
                continue;
            }

            if (const auto *booleanValue = std::get_if<bool>(&parameterValue))
            {
                // MySQL 没有独立的布尔存储类，TINYINT(1) 是官方约定。
                // MYSQL_TYPE_TINY 要求缓冲区元素类型与之一致（有符号单字节），因此先落到自己的存储再交出地址
                tinyValues[index]   = *booleanValue ? 1 : 0;
                binding.buffer_type = MYSQL_TYPE_TINY;
                binding.buffer      = asBindBuffer(&tinyValues[index]);
                continue;
            }

            if (const auto *integerValue = std::get_if<std::int64_t>(&parameterValue))
            {
                // 有符号 64 位整数直连缓冲区：is_unsigned 留默认假，BIGINT SIGNED 与 int64_t 一一对应
                binding.buffer_type = MYSQL_TYPE_LONGLONG;
                binding.buffer      = asBindBuffer(integerValue);
                continue;
            }

            if (const auto *realValue = std::get_if<double>(&parameterValue))
            {
                binding.buffer_type = MYSQL_TYPE_DOUBLE;
                binding.buffer      = asBindBuffer(realValue);
                continue;
            }

            if (const auto *textValue = std::get_if<std::string>(&parameterValue))
            {
                // 文本按「指针 + 长度」绑定，内嵌 '\0' 因此不丢。长度同时写进 buffer_length（缓冲区容量）
                // 与 *length（客户端库实际采用的输入长度）；超长文本会被 unsigned long 形参静默截断，直接拒绝
                if (textValue->size() > kMaximumNativeLength)
                {
                    m_lastError = parameterTooLongText(index, false, textValue->size(), "MySQL");
                    return false;
                }

                parameterLengths[index] = static_cast<unsigned long>(textValue->size());
                binding.buffer_type     = MYSQL_TYPE_STRING;
                binding.buffer          = asBindBuffer(textValue->data());
                binding.buffer_length   = parameterLengths[index];
                binding.length          = &parameterLengths[index];
                continue;
            }

            if (const auto *byteValue = std::get_if<BinaryBytes>(&parameterValue))
            {
                // 二进制必须走 MYSQL_TYPE_BLOB：若按 MYSQL_TYPE_STRING 下发，服务端会把载荷当作
                // 连接字符集（本连接是 utf8mb4）下的字符串，非该字符集的字节序列可能被替换或直接
                // 报错——写进去和读回来就不是同一串字节了。类型上是 BLOB，服务端便不再做字符集转换
                if (byteValue->size() > kMaximumNativeLength)
                {
                    m_lastError = parameterTooLongText(index, true, byteValue->size(), "MySQL");
                    return false;
                }

                parameterLengths[index] = static_cast<unsigned long>(byteValue->size());
                binding.buffer_type     = MYSQL_TYPE_BLOB;
                // 空载荷指向静态字节而不是空指针：buffer 为空时客户端库行为无保证，
                // 而长度为 0 已足够表达「零长度 BLOB」
                binding.buffer        = asBindBuffer(byteValue->empty() ? &kEmptyBinaryPayloadByte : byteValue->data());
                binding.buffer_length = parameterLengths[index];
                binding.length        = &parameterLengths[index];
                continue;
            }

            // 容器的正确用法是展开成多个标量参数（如 IN 列表），而不是当成单个参数：
            // 方言层已把 IN 集合展开成多个占位符，走到这里说明调用方传了非标量值
            m_lastError = containerParameterRejectedText(index, parameterValue);
            return false;
        }

        // 零参数时不需要下发绑定：传空指针给第三方库虽然通常可行，但它的行为在文档里没有明确保证，
        // 而且此时 MYSQL_BIND 数组本就是空的（data() 可能是空指针），直接跳过最稳妥
        if (!bindings.empty())
        {
            if (mysql_stmt_bind_param(statement, bindings.data()) != 0)
            {
                captureStatementError(statement, "绑定预处理语句参数失败");
                return false;
            }
        }

        if (mysql_stmt_execute(statement) != 0)
        {
            // 错误码要在摘文本之前抓：两者都指向语句句柄里的同一份状态，顺序不影响取值，
            // 但都必须早于调用方关闭语句
            const unsigned int errorNumber = mysql_stmt_errno(statement);
            captureStatementError(statement, "执行预处理语句失败");

            // CR_SERVER_GONE_ERROR / CR_SERVER_LOST 代表链路已断，这个连接再也无法复用；
            // 顺手断开并释放，让 isConnected() 与后续调用一致地看到「未连接」，调用方重连即可
            if (errorNumber == CR_SERVER_GONE_ERROR || errorNumber == CR_SERVER_LOST)
            {
                disconnect();
            }

            return false;
        }

        return true;
    }

    std::unique_ptr<DatabaseResult> MySqlConnection::materializePreparedResult(MYSQL_STMT *const statement)
    {
        // 列元数据是一份独立的 MYSQL_RES（只有列定义、没有行数据），用完同样要 mysql_free_result。
        // 必须在 store_result 之后取：打开 STMT_ATTR_UPDATE_MAX_LENGTH 时，store_result 会把每列的最长值
        // 长度写进这份元数据，下面的缓冲区分配正是靠它精确确定大小
        MYSQL_RES *rawMetadata = mysql_stmt_result_metadata(statement);
        if (rawMetadata == nullptr)
        {
            captureStatementError(statement, "读取预处理语句的列元数据失败");
            return nullptr;
        }
        std::unique_ptr<MYSQL_RES, ResultReleaser> guardedMetadata{rawMetadata};

        const auto columnCount = static_cast<std::size_t>(mysql_num_fields(rawMetadata));
        // 这里要非 const 的字段数组：本轮取完数据后得把每列的 max_length 归零（见循环后的说明）
        MYSQL_FIELD *fields = mysql_fetch_fields(rawMetadata);
        if (fields == nullptr && columnCount > 0)
        {
            // 有列却拿不到元数据：无法确定列名与列类型，构造出来的结果集只会误导调用方
            m_lastError = "读取 MySQL 结果集失败：列元数据不可用";
            return nullptr;
        }

        std::vector<std::string> columnNames;
        columnNames.reserve(columnCount);

        // 取值缓冲按列分配：外层容器一次性定型，之后只改内层内容，
        // 因此各列缓冲区首地址在整个预读过程中保持稳定（绑定指针只在开始时取一次）
        std::vector<std::vector<char>> columnBuffers(columnCount);
        std::vector<unsigned long>     columnLengths(columnCount, 0UL);
        std::vector<int>               columnTypes(columnCount, 0);
        // 字符集号是二进制列与文本列的唯一区分依据（两者在协议层共用同一个类型码），
        // 因此必须与类型码一起缓存下来供逐列转换使用
        std::vector<unsigned int> columnCharacterSets(columnCount, 0U);

        // MYSQL_BIND 的 is_null 形参类型是 bool*，而 std::vector<bool> 是位压缩的、取不到元素地址，
        // 因此用 make_unique 动态分配一段定长 bool 数组（不是裸 new）
        const std::unique_ptr<bool[]> columnNullFlags = std::make_unique<bool[]>(columnCount);

        std::vector<MYSQL_BIND> resultBindings(columnCount);
        for (std::size_t index = 0; index < columnCount; ++index)
        {
            const MYSQL_FIELD &field = fields[index];
            // 表达式列在部分客户端版本上可能没有名字，补空串占位，保证列名列表长度与列数严格对齐
            columnNames.emplace_back(field.name != nullptr ? field.name : "");
            columnTypes[index]         = static_cast<int>(field.type);
            columnCharacterSets[index] = static_cast<unsigned int>(field.charsetnr);

            // max_length 为 0 表示整列都是 NULL 或空串，此时取 1 字节只为拿到合法指针
            const unsigned long bufferBytes = field.max_length > kMinimumColumnBufferBytes ? field.max_length : kMinimumColumnBufferBytes;
            columnBuffers[index].assign(static_cast<std::size_t>(bufferBytes), '\0');

            MYSQL_BIND &binding = resultBindings[index];
            // 一律按字符串缓冲取值（MySQL 会把数值、日期等列转成文本写进缓冲区），再按列声明类型解析；
            // 这与文本协议路径「按 (指针, 长度) 拿字节 + 按列类型解析」完全同构，
            // 两条路径的取值映射因此不会出现分歧（列类型到 DatabaseValue 的规则见 MySqlValueConversion.h）
            binding.buffer_type   = MYSQL_TYPE_STRING;
            binding.buffer        = columnBuffers[index].data();
            binding.buffer_length = bufferBytes;
            binding.length        = &columnLengths[index];
            binding.is_null       = &columnNullFlags[index];
        }

        if (mysql_stmt_bind_result(statement, resultBindings.data()) != 0)
        {
            captureStatementError(statement, "绑定预处理语句结果缓冲区失败");
            return nullptr;
        }

        // 行数已由 store_result 全部取回，num_rows 是精确值，按它预留容量避免反复扩容
        std::vector<std::vector<DatabaseValue>> rows;
        rows.reserve(static_cast<std::size_t>(mysql_stmt_num_rows(statement)));

        while (true)
        {
            const int fetchResult = mysql_stmt_fetch(statement);
            if (fetchResult == MYSQL_NO_DATA)
            {
                break;
            }
            if (fetchResult == 1)
            {
                // 预读过的结果集在这里出错只可能是客户端库内部异常，如实报出并放弃整份结果
                captureStatementError(statement, "读取预处理语句结果行失败");
                return nullptr;
            }

            std::vector<DatabaseValue> currentRow;
            currentRow.reserve(columnCount);
            for (std::size_t index = 0; index < columnCount; ++index)
            {
                if (columnNullFlags[index])
                {
                    // 列值为 SQL NULL：与「空串」「0」是三件不同的事，只有 NULL 才映射成 monostate
                    currentRow.emplace_back(std::monostate{});
                    continue;
                }

                if (columnLengths[index] > columnBuffers[index].size())
                {
                    // 缓冲区按 store_result 更新过的 max_length 分配，正常路径不会走到这里。
                    // 万一客户端库没有按属性更新长度，就按实际长度单独补取这一列——
                    // 这是文档给出的长数据读取方式（截断后可按列重取当前行），
                    // 绝不把被截断的数据交给调用方
                    const unsigned long actualLength = columnLengths[index];
                    std::vector<char>   exactBuffer(static_cast<std::size_t>(actualLength));

                    MYSQL_BIND columnBinding{};
                    // 补取也要按列的真实性质选类型：二进制列若按 MYSQL_TYPE_STRING 重取，
                    // 客户端库会按字符集转换一次，拿回来的就不再是原始字节
                    columnBinding.buffer_type   = Detail::isBinaryColumn(columnTypes[index], columnCharacterSets[index]) ? MYSQL_TYPE_BLOB : MYSQL_TYPE_STRING;
                    columnBinding.buffer        = exactBuffer.data();
                    columnBinding.buffer_length = actualLength;
                    columnBinding.length        = &columnLengths[index];

                    if (mysql_stmt_fetch_column(statement, &columnBinding, static_cast<unsigned int>(index), 0) != 0)
                    {
                        captureStatementError(statement, "补取预处理语句结果列失败");
                        return nullptr;
                    }

                    currentRow.push_back(Detail::convertColumnText(columnTypes[index], columnCharacterSets[index], exactBuffer.data(), static_cast<std::size_t>(actualLength)));
                    continue;
                }

                currentRow.push_back(
                        Detail::convertColumnText(columnTypes[index], columnCharacterSets[index], columnBuffers[index].data(), static_cast<std::size_t>(columnLengths[index])));
            }

            rows.push_back(std::move(currentRow));
        }

        // 把每列的 max_length 归零再交出去。它记的不是「本次结果的最长值」而是「这条语句见过的最长值」：
        // 客户端库按 max(已有, 本次观测) 累积，而这份字段数组跟着缓存语句长期活着。不归零的话，一条
        // 先读过大 BLOB 的缓存语句会让之后每次小载荷查询都按那个高水位申请并清零缓冲（实测一次 20 字节
        // 的读取要申请 1,049,137 字节）。归零后每次执行的累积值就等于本次自己的最长值，缓冲跟着本次数据收缩
        for (std::size_t index = 0; index < columnCount; ++index)
        {
            fields[index].max_length = 0UL;
        }

        // 行数据已全部搬到快照里，语句与元数据在本方法返回后由守卫释放，快照不引用任何句柄
        return std::make_unique<MySqlStatementResult>(std::move(columnNames), std::move(rows));
    }

#else // DATABASE_HAS_MYSQL —— 桩实现：CMake 未找到 libmysqlclient 时编译，所有入口明确失败

    namespace
    {
        // 桩构建的统一失败原因：每个入口都把它写进 lastError()，
        // 避免调用方把「什么都没做」当成成功
        constexpr const char *kMissingDriverError = "当前构建未编译 MySQL 驱动（缺少 libmysqlclient）";
    } // namespace

    MySqlConnection::MySqlConnection(const ConnectionConfig &configuration)
    {
        // 桩同样只登记配置，不做任何分配与 IO。构造本身是成功的，
        // 因此不提前占用 lastError()——缺失驱动的提示由各入口在真正要用时给出
        m_configuration = configuration;
    }

    bool MySqlConnection::connect()
    {
        m_lastError = kMissingDriverError;
        return false;
    }

    void MySqlConnection::disconnect()
    {
        // 没有句柄可释放，只把状态归位，保证析构路径无条件调用本方法是安全的
        m_isConnected = false;
    }

    bool MySqlConnection::isConnected() const
    {
        return false;
    }

    std::unique_ptr<DatabaseResult> MySqlConnection::execute(const std::string_view)
    {
        m_lastError = kMissingDriverError;
        return nullptr;
    }

    std::unique_ptr<DatabaseResult> MySqlConnection::execute(const std::string_view, const std::span<const DatabaseValue>)
    {
        // 参数化路径与不带参数的路径在桩里没有区别：连客户端库都没有，既无法预处理也无法绑定参数。
        // 明确报出「驱动缺失」而不是基类默认的「暂不支持参数化查询」，
        // 否则使用者会以为问题出在「这个驱动没实现参数绑定」而不是「当前构建没编译驱动」
        m_lastError = kMissingDriverError;
        return nullptr;
    }

    std::string MySqlConnection::serverVersion() const
    {
        // 桩里没有客户端库可问，返回空串（含义与「未连接」一致，调用方本就连不上）
        return {};
    }

    void MySqlConnection::applyQueryTimeoutNow() noexcept
    {
        // 桩里既没有会话也没有服务端可变可设，空操作即可：基类已把毫秒值记好，
        // 真驱动接上时自会按它下发。本方法声明为虚，因此即使没人调用也需要这份定义把虚表槽链上
    }

#endif // DATABASE_HAS_MYSQL

    // ------------------------------------------------------------------------
    // 以下定义与是否编译 libmysqlclient 无关，两种构建配置共用
    // ------------------------------------------------------------------------

    MySqlConnection::~MySqlConnection()
    {
        // RAII 收尾：析构阶段虚表已回到本类，直接调用 disconnect() 而不经虚接口，
        // 保证无论调用方是否显式断开都不会漏掉 mysql_close。
        // 注意虚析构只能在类内首次声明处 = default，本函数是类外定义，必须给出函数体
        MySqlConnection::disconnect();
    }

    DatabaseType MySqlConnection::databaseType() const
    {
        return DatabaseType::MySql;
    }

    bool MySqlConnection::beginTransaction()
    {
        // 语句文本取自方言而不是写死在这里：同一件事（开启事务）若有两份语句源，
        // 就会随方言演进而漂移，与 Transaction 走方言的路径产生行为差异
        const MySqlDialect dialect;
        // 事务语句没有返回列，execute() 非空即代表 START TRANSACTION 已被服务端接受
        const bool isStarted = execute(dialect.beginTransactionStatement()) != nullptr;
        m_isTransactionOpen  = isStarted;
        return isStarted;
    }

    bool MySqlConnection::commit()
    {
        const MySqlDialect dialect;
        const bool         isCommitted = execute(dialect.commitStatement()) != nullptr;
        if (isCommitted)
        {
            m_isTransactionOpen = false;
        }
        return isCommitted;
    }

    bool MySqlConnection::rollback()
    {
        const MySqlDialect dialect;
        // 没有活动事务时服务端会直接报错，这里如实返回 false，并由 lastError() 给出原因
        const bool isRolledBack = execute(dialect.rollbackStatement()) != nullptr;
        if (isRolledBack)
        {
            m_isTransactionOpen = false;
        }
        return isRolledBack;
    }

    void MySqlConnection::resetSessionState() noexcept
    {
        // 未连接，或本类没开过事务：没有要复位的东西（记账范围见头文件说明）
        if (m_mysqlHandle == nullptr || !m_isTransactionOpen)
        {
            return;
        }

        // 先清标记再滚：即便这次 ROLLBACK 发不出去（链路已断），连接也不会带着「还开着事务」
        // 的假状态回到池里。失败只记在 lastError() 里（与 rollback() 同一口径），
        // 归还路径不看返回码——绝不在这里抛异常打断归还
        m_isTransactionOpen = false;
        // rollback() 会构造 std::string（内存分配失败即抛），而本方法按接口约定是 noexcept：
        // 不接住就是 terminate，因此显式吞掉，失败只留在 lastError() 里
        try
        {
            [[maybe_unused]] const bool isRolledBack = rollback();
        } catch (...)
        {
            // 归还路径绝不抛出：最坏情况是连接带着未复位的事务回到池里
        }
    }

} // namespace AsynGyanis::Database
