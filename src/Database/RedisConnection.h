/**
 * @file RedisConnection.h
 * @brief Redis 数据库连接实现
 * @copyright Copyright (c) 2026
 */

#ifndef DATABASE_REDISCONNECTION_H
#define DATABASE_REDISCONNECTION_H

#include "DatabaseConnection.h"
#include "DatabaseResult.h"
#include "DatabaseType.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// 前向声明 hiredis 结构体
struct redisContext;

namespace Database
{
    /**
     * @brief Redis 数据库连接
     *
     * 封装 hiredis C 库，实现 DatabaseConnection 抽象接口。
     * 支持单命令执行、管道（Pipeline）批量命令和发布订阅（Pub/Sub）。
     *
     * 底层依赖 hiredis，通过 Conan 或系统包管理安装。
     *
     * 使用方式：
     * @code
     *   auto config = ConnectionConfig::redisDefault();
     *   RedisConnection connection(config);
     *   if (connection.connect()) {
     *       auto result = connection.execute("GET mykey");
     *       auto value  = result->getValue(0);
     *
     *       connection.execute("SET mykey newvalue");
     *   }
     * @endcode
     */
    class RedisConnection : public DatabaseConnection
    {
    public:
        /**
         * @brief 使用配置构造 Redis 连接
         * @param config 连接配置
         */
        explicit RedisConnection(const ConnectionConfig &config);

        /**
         * @brief 析构时自动断开连接
         */
        ~RedisConnection() override;

        RedisConnection(const RedisConnection &)            = delete;
        RedisConnection &operator=(const RedisConnection &) = delete;
        RedisConnection(RedisConnection &&)                 = default;
        RedisConnection &operator=(RedisConnection &&)      = default;

        bool connect() override;
        void disconnect() override;
        [[nodiscard]] bool isConnected() const override;
        std::unique_ptr<DatabaseResult> execute(std::string_view command) override;
        [[nodiscard]] DatabaseType databaseType() const override;
        [[nodiscard]] std::string lastError() const override;

        /**
         * @brief 执行 Redis 命令（可变参数版本）
         * @param arguments 命令参数列表，第一个为命令名
         * @return 结果集智能指针
         */
        std::unique_ptr<DatabaseResult> executeCommand(
            const std::vector<std::string_view> &arguments);

        /**
         * @brief 发送命令到管道（Pipeline 模式）
         *
         * 在管道模式下，命令被缓存而不立即发送。调用 flushPipeline() 时
         * 批量发送所有缓存的命令。使用管道可以显著减少网络往返次数。
         *
         * @param command Redis 命令字符串
         */
        void pipelineCommand(std::string_view command);

        /**
         * @brief 刷新管道，批量发送所有缓存的命令
         * @return 所有命令的结果集列表
         */
        std::vector<std::unique_ptr<DatabaseResult>> flushPipeline();

        /**
         * @brief 选择指定的 Redis 数据库实例
         * @param index 数据库索引（0-15）
         * @return 成功返回 true
         */
        bool selectDatabase(int index);

        /**
         * @brief 获取底层 redisContext 句柄（仅供内部或高级用途）
         * @return redisContext 指针
         */
        [[nodiscard]] redisContext *nativeHandle() const { return m_redisContext; }

    private:
        /**
         * @brief 解析 Redis 错误信息并存入 m_lastError
         */
        void captureError();

        redisContext             *m_redisContext{nullptr}; ///< hiredis 连接上下文
        std::vector<std::string>  m_pipelineBuffer;        ///< 管道命令缓冲区
        bool                      m_pipelineMode{false};   ///< 是否处于管道模式
    };

} // namespace Database

#endif // DATABASE_REDISCONNECTION_H
