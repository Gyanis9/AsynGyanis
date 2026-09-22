/**
 * @file RedisConnection.h
 * @brief Redis 键值存储连接实现
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
#include <vector>

// hiredis 的 redisContext 与 redisReply 都是全局作用域的 C 结构体，前置声明集中写在本头的全局作用域：
// 只有 .cpp 才包含 <hiredis/hiredis.h>，避免第三方 C 头顺着包含链传染给所有使用方。
// RedisResult.h 通过包含本头复用这两行声明，不得重复声明。
struct redisContext;
struct redisReply;

namespace AsynGyanis::Database
{
    /**
     * @brief Redis 键值存储连接
     *
     * @details 可选编译：未取得 hiredis 时编译为报错桩（各执行入口把「当前构建未编译 Redis 驱动」写入
     *          lastError()）。命令一律走 hiredis 的 argv 接口，参数按「指针 + 长度」传递，因此 '%' 不是
     *          格式串、内嵌 '\0' 不被截断；execute() 的整行命令按 redis-cli 规则切词后送出。queryTimeout()
     *          在建连后由 redisSetTimeout 应用到上下文，因此 AUTH 与初始 SELECT 也在读写超时保护内。
     *
     * @warning 管道命令登记后不立即发送，flushPipeline() 之前不会有任何网络往返；
     *          中途的传输层失败会丢弃尚未读回的回复并断开连接，Redis 侧无法回滚已执行的命令。
     * @warning 不支持订阅式用法（SUBSCRIBE/PSUBSCRIBE/监视模式）：本类按「一条命令一次读回复」
     *          的模型执行，一旦对端切到推送模式，后续回复会与命令错位——调用方若需要订阅，
     *          请自行使用 hiredis 的异步 API。
     */
    class RedisConnection : public DatabaseConnection
    {
    public:
        /**
         * @brief 使用配置构造 Redis 连接，此阶段不发生任何网络交互
         * @param configuration 连接配置；host/port 用于建连，password/userName 用于认证，
         *                      database 作为键空间编号在 connect() 内被 SELECT
         */
        explicit RedisConnection(const ConnectionConfig &configuration);

        /**
         * @brief 析构时自动断开连接，释放 hiredis 上下文
         */
        ~RedisConnection() override;

        // 上下文所有权唯一：拷贝会出现两个对象 redisFree 同一个 redisContext；
        // 移动会让源对象析构时再次 redisFree（同样的句柄已交给目标对象），
        // 因此拷贝与移动一律禁止（基类同样已删除，这里显式写清意图）。
        RedisConnection(const RedisConnection &) = delete;

        RedisConnection &operator=(const RedisConnection &) = delete;

        RedisConnection(RedisConnection &&) = delete;

        RedisConnection &operator=(RedisConnection &&) = delete;

        /**
         * @brief 连接 Redis 服务并完成认证与键空间选择
         * @details 重写 DatabaseConnection::connect()：已连接时直接返回 true 保持幂等（重复
         *          redisConnectWithTimeout 会泄漏前一个上下文）；建连成功后用 redisSetTimeout 应用
         *          queryTimeout()；password / database 非空时依次发送二进制安全的 AUTH 与 SELECT，任一步
         *          失败即断开；全部成功后 m_isConnected 才置位。其余与基类一致；桩构建下直接返回 false。
         * @return true 连接已建立（含认证与键空间选择）
         * @return false 任一环节失败，原因见 lastError()
         * @note host 为空视为配置错误，直接失败而不是交给 hiredis 报出难懂的底层错误
         */
        bool connect() override;

        /**
         * @brief 断开连接并释放 hiredis 上下文
         * @details 重写 DatabaseConnection::disconnect()：与基类的差异是会一并丢弃管道缓冲区——
         *          这些命令尚未发送或尚未读回，重连后继续发送会把它们插入到另一条会话中间，
         *          造成服务端状态无法预期的批量写入。
         *          未连接且无残留上下文时是安全的空操作，析构函数会无条件调用本方法。
         * @note 已交出的 RedisResult 不依赖本连接，断开后仍可正常读取
         */
        void disconnect() override;

        /**
         * @brief 判断连接是否可用
         * @details 重写 DatabaseConnection::isConnected()：不做 PING 之类的活性探测（一次往返的代价对
         *          高频命令不可接受），只同时校验 m_isConnected 与 hiredis 上下文非空；链路被对端单方面
         *          断开时这里仍返回 true，真实失效由下一条命令的失败路径发现并断开。其余与基类一致。
         * @return true 已连接且上下文有效
         */
        [[nodiscard]] bool isConnected() const override;

        // 引入基类的全部 execute 重载：本类声明了名为 execute 的成员，按 C++ 名字查找规则
        // 会隐藏基类的同名重载，加上这一行后通过具体对象也能调用参数化版本。
        // Redis 不是 SQL 数据库，参数化版本由基类默认实现返回中文错误提示，
        // 需要按参数发送命令请改用 executeCommand()
        using DatabaseConnection::execute;

        /**
         * @brief 执行一条 Redis 命令
         * @details 重写 DatabaseConnection::execute()：每次调用开头清空 m_lastError；整行命令按 redis-cli
         *          规则切词后走 argv 接口（参数含空格必须加引号，'%' 不再有格式串含义）；引号未闭合或整行无
         *          有效参数时判定为命令不合法、直接失败且不发送任何字节；服务端 error 回复返回 nullptr 并把
         *          原文写入 lastError()；传输层失败顺带断开连接。其余与基类一致。
         * @param command 命令文本，例如 "SET mykey myvalue"
         * @return std::unique_ptr<DatabaseResult> 结果集；失败返回 nullptr，原因见 lastError()
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> execute(std::string_view command) override;

        /**
         * @brief 获取数据库类型
         * @details 重写 DatabaseConnection::databaseType()：恒定返回 DatabaseType::Redis，桩构建下同样返回本类型。
         * @return DatabaseType DatabaseType::Redis
         */
        [[nodiscard]] DatabaseType databaseType() const override;

        /**
         * @brief 以参数数组形式执行一条 Redis 命令
         * @details 与 execute() 共用同一条发送路径，区别在于参数已经切分好，
         *          因此不需要引号转义，也不必担心参数里含空白：
         *          executeCommand({"SET", "my key", "a\nb"}) 会正确发出三个参数。
         *          参数以「指针 + 长度」交给 hiredis，内嵌 '\0' 不丢失。
         * @param arguments 参数列表，第一个元素为命令名，其余为其参数
         * @return std::unique_ptr<DatabaseResult> 结果集；参数为空、未连接或命令失败返回 nullptr，原因见 lastError()
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> executeCommand(const std::vector<std::string_view> &arguments);

        /**
         * @brief 登记一条管道命令（不立即发送）
         * @details 命令在此处切词并登记进缓冲区，真正的网络往返发生在 flushPipeline()。
         *          批量管道能把 N 次往返压成一次，是 Redis 上最重要的吞吐优化。
         *          不另设「是否处于管道模式」标志：缓冲区非空即代表有命令待发，flush 之后一律为空。
         * @param command 命令文本，切词规则与 execute() 一致
         * @return true 命令已登记
         * @return false 命令不合法（内容为空或引号未闭合），不会登记半个参数，原因见 lastError()
         * @note 未连接时允许登记（此处无 IO），由 flushPipeline() 统一报错并丢弃
         */
        bool pipelineCommand(std::string_view command);

        /**
         * @brief 归还连接池时丢掉未发送的管道命令
         * @details 管道是「登记到 flush 之间」的会话状态：上一个借用者没 flush 就归还时，
         *          残留命令会被下一个借用者的 flushPipeline() 代发，回复按下标错位且毫无报错。
         *          池在归还时统一调用本方法（见 DatabaseConnection::resetSessionState）
         */
        void resetSessionState() noexcept override;

        /**
         * @brief 一次性发送全部已登记的管道命令并读回回复
         * @details 先逐条 append（此时才开始发送），再按已发出的条数读回复，因此返回顺序与登记顺序严格
         *          一致；返回的元素永不为 nullptr。服务端 error 回复被原样封装成 RedisResult 返回，由调用方
         *          用 isError() 定位哪一条失败（连接级 lastError() 表达不了「N 条中的第几条」）。传输层错误时
         *          记录原因、丢弃未读回的命令并断开连接，返回已取到的前缀，已登记的命令一律不重放到新连接。
         * @return std::vector<std::unique_ptr<DatabaseResult> > 与已发送命令一一对应（截断后）的结果集列表
         */
        [[nodiscard]] std::vector<std::unique_ptr<DatabaseResult> > flushPipeline();

        /**
         * @brief 切换键空间（数据库编号）
         * @details 等价于 executeCommand({"SELECT", 编号})。Redis 默认有 16 个键空间（0..15），
         *          服务端也可用 databases 配置项改数量：本方法只校验非负，
         *          超出范围的编号由服务端报错，并按 executeCommand 的失败路径如实返回 false。
         * @param index 键空间编号，必须为非负整数
         * @return true 切换成功（服务端回了非 error 回复）
         * @return false 编号非法、未连接或服务端拒绝，原因见 lastError()
         */
        bool selectDatabase(int index);

        /**
         * @brief 获取底层 redisContext 句柄，供需要直接使用 hiredis 的高级场景使用
         * @warning 句柄所有权始终属于本连接，调用方不得 redisFree，
         *          也不得在连接销毁后继续使用；拿它去发命令会绕过本类的错误与超时处理
         * @return redisContext* 未连接时为 nullptr；桩构建下恒为 nullptr
         */
        [[nodiscard]] redisContext *nativeHandle() const noexcept
        {
            return m_redisContext;
        }

    private:
        /**
         * @brief 采集 hiredis 上下文的 errstr 与 err 码并写入 m_lastError
         * @param description 面向使用者的中文动作说明，例如「执行 Redis 命令失败」
         */
        void captureError(std::string_view description);

        /**
         * @brief 把 queryTimeout() 应用到已建立的上下文
         * @return true 已应用（含「非正值表示取消超时」这一合法情形）
         * @return false redisSetTimeout 失败，m_lastError 已记录原因
         */
        bool applyQueryTimeout();

        /**
         * @brief 用已切分好的参数数组发送一条命令并封装结果（execute/executeCommand 的共同路径）
         * @param argumentValues 参数数组，第一个元素为命令名，调用期间必须保持存活且不被修改。
         *        按视图接收：hiredis 的 argv 接口要的就是「指针 + 长度」，把参数落回 std::string
         *        只会为每个参数多取一次堆块，而内嵌 '\0' 照样靠长度安全穿过
         * @return std::unique_ptr<DatabaseResult> 结果集；传输失败或服务端报错时返回 nullptr
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> executeArguments(std::span<const std::string_view> argumentValues);

        redisContext *m_redisContext{nullptr}; ///< hiredis 连接上下文，本对象独占所有权，未连接时为 nullptr

        // 之所以在登记时就切词而不是原样缓存命令文本：命令文本的合法性错误能在 pipelineCommand()
        // 当场反馈，不必等到 flush 时才发现「N 条里有一条引号没闭合」
        std::vector<std::vector<std::string> > m_pipelineCommands; ///< 管道命令缓冲区，元素是已切词好的参数数组
    };

} // namespace AsynGyanis::Database
