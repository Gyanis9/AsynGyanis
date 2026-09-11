/**
 * @file MySqlConnection.cpp
 * @brief MySQL / MariaDB 数据库连接实现
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Database/MySql/MySqlConnection.h"

#include "Database/MySql/MySqlResult.h"

#ifdef DATABASE_HAS_MYSQL

// libmysqlclient / MariaDB Connector/C 的头文件只在本实现文件里包含，对外只暴露 MySqlConnection.h 的前置声明。
// 这两个头自带平台网络头的包含顺序，调用方无需先包含 winsock2.h 之类
#include <mysql/mysql.h>
// CR_SERVER_GONE_ERROR / CR_SERVER_LOST 等连接级错误码定义在这里（旧实现包含了却从未用到）
#include <mysql/errmsg.h>

#include <limits>

#endif // DATABASE_HAS_MYSQL

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace AsynGyanis::Database
{
#ifdef DATABASE_HAS_MYSQL

    namespace
    {
        // 用 constexpr 常量取代宏：单位换算与协议上限的出处集中在此，类型安全且作用域受控
        constexpr int kMillisecondsPerSecond = 1000; ///< 1 秒等于 1000 毫秒，毫秒→秒换算的唯一依据

        // 连接字符集：utf8mb4 才能真正存下 emoji 与全部 BMP 之外的汉字（utf8mb3 不行）
        constexpr const char *kConnectionCharacterSet = "utf8mb4";

        // mysql_real_query 的长度形参是 unsigned long：Windows(LLP64) 上它是 32 位，
        // 超过上限的命令会被静默截断成半条语句，宁可报错也不执行残缺命令
        constexpr size_t kMaximumCommandLength = static_cast<size_t>(std::numeric_limits<unsigned long>::max());

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
            const unsigned int positiveMilliseconds = static_cast<unsigned int>(milliseconds);
            return (positiveMilliseconds + static_cast<unsigned int>(kMillisecondsPerSecond) - 1U) /
                   static_cast<unsigned int>(kMillisecondsPerSecond);
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
        // clientflag 传 0：不开 CLIENT_MULTI_STATEMENTS（一次一条语句），也不开 LOCAL_INFILE
        if (mysql_real_connect(m_mysqlHandle,
                               m_configuration.host.c_str(),
                               m_configuration.userName.c_str(),
                               m_configuration.password.c_str(),
                               m_configuration.database.c_str(),
                               m_configuration.port,
                               nullptr,
                               0) == nullptr)
        {
            // 先摘 mysql_error 的文本再 disconnect：mysql_close 会释放错误缓冲，顺序反了就读到悬垂指针
            captureError("连接 MySQL 服务失败");
            disconnect();
            return false;
        }

        // 全部步骤走通才置位：中途任何一步失败都不会让 isConnected() 读到「已连接」的中间态
        m_isConnected = true;
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
        // 这里不清 m_lastError——失败路径上先写好的原因不能被断开动作抹掉
        m_isConnected = false;

        if (m_mysqlHandle == nullptr)
        {
            return;
        }

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
            if (command.size() > kMaximumCommandLength)
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
            // 属于「执行成功的空回执」，不是错误：交出一个 0 行 0 列的结果集，让调用方只需判 nullptr
            if (mysql_field_count(m_mysqlHandle) == 0)
            {
                return std::make_unique<MySqlResult>(nullptr);
            }

            // 有返回列却没拿到结果集：通常是预读途中内存不足，回复流的位置已不可知，这条连接不能再用于发命令。
            // 依旧是先摘文本、再断开
            captureError("读取 MySQL 结果集失败");
            disconnect();
            return nullptr;
        }

        // 用带自定义释放器的 unique_ptr 罩住这段所有权真空：make_unique 若因内存分配失败抛异常，
        // MYSQL_RES 会由守卫释放，而不是像旧实现那样直接泄漏（旧代码把裸指针交给构造函数后再无兜底）
        std::unique_ptr<MYSQL_RES, ResultReleaser> guardedResult{rawResult};
        std::unique_ptr<MySqlResult> result = std::make_unique<MySqlResult>(guardedResult.get());

        // 构造成功，所有权正式移交结果集：此后再由 MySqlResult 的析构负责 mysql_free_result
        guardedResult.release();
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
        // 因此局部变量随本函数结束析构是安全的（旧实现传的是基类 int 成员的地址，单位与类型双双错位）
        const unsigned int connectionTimeoutSeconds = toClientSeconds(connectTimeout());
        const unsigned int ioTimeoutSeconds         = toClientSeconds(queryTimeout());

        // 逐条下发，任一选项被拒就整体判失败——「超时静默不生效」正是旧实现的核心缺陷，
        // 宁可连不上也不留一条没有超时保护的会话
        const auto applyOption = [this](const mysql_option option, const void *argumentValue, const std::string_view description) -> bool {
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

        // 写超时：旧实现漏掉了这一项，网络半断时发送命令可以一直卡在 socket write 上。
        // 读写共用 queryTimeout()：一条命令的预算本就该覆盖「发出去 + 读回来」整个来回
        if (!applyOption(MYSQL_OPT_WRITE_TIMEOUT, &ioTimeoutSeconds, "设置 MySQL 写超时"))
        {
            return false;
        }

        // 字符集在握手期一次定成 utf8mb4：服务端不认识该字符集时这里就会失败，
        // 好过连上之后中文列被按 latin1 解读成乱码
        return applyOption(MYSQL_SET_CHARSET_NAME, kConnectionCharacterSet, "设置 MySQL 连接字符集");
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
        const char *rawMessage         = mysql_error(m_mysqlHandle);
        std::string serverMessage(rawMessage != nullptr ? rawMessage : "");
        if (serverMessage.empty())
        {
            // 错误码非 0 但文本为空是客户端库的已知退化情形（例如某些选项被拒），给出可读兜底
            serverMessage = "客户端库未给出原因";
        }

        // 带上错误码：只留一句中文 + 服务端英文原文时，排查 1045 / CR_* 之类问题仍需原始数字
        m_lastError = std::string(description) + "：" + serverMessage + "（错误码 " + std::to_string(errorNumber) + "）";
    }

#else // DATABASE_HAS_MYSQL —— 桩实现：CMake 未找到 libmysqlclient 时编译，所有入口明确失败

    namespace
    {
        // 桩构建的统一失败原因：旧桩让 connect() 静默返回、execute() 只回 nullptr 而不写原因，
        // 调用方会把「什么都没做」当成成功，这里每个入口都把它写成看得见的错误
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

    std::string MySqlConnection::serverVersion() const
    {
        // 桩里没有客户端库可问，返回空串（含义与「未连接」一致，调用方本就连不上）
        return {};
    }

    bool MySqlConnection::applyConnectionOptions()
    {
        // 没有句柄可下发选项；connect() 已经先行失败返回，正常路径走不到这里
        m_lastError = kMissingDriverError;
        return false;
    }

    void MySqlConnection::captureError(const std::string_view)
    {
        // 桩构建里没有句柄可采集，一律按「驱动缺失」定性
        m_lastError = kMissingDriverError;
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
        disconnect();
    }

    DatabaseType MySqlConnection::databaseType() const
    {
        return DatabaseType::MySql;
    }

    bool MySqlConnection::beginTransaction()
    {
        // 事务语句没有返回列，execute() 非空即代表 START TRANSACTION 已被服务端接受
        return execute("START TRANSACTION") != nullptr;
    }

    bool MySqlConnection::commit()
    {
        return execute("COMMIT") != nullptr;
    }

    bool MySqlConnection::rollback()
    {
        // 没有活动事务时服务端会直接报错，这里如实返回 false，并由 lastError() 给出原因
        return execute("ROLLBACK") != nullptr;
    }

} // namespace AsynGyanis::Database
