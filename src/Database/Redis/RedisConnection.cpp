/**
 * @file RedisConnection.cpp
 * @brief Redis 连接实现
 * @copyright Copyright (c) 2026
 */

#include "RedisConnection.h"
#include "RedisResult.h"

#ifdef DATABASE_HAS_REDIS
#include <hiredis/hiredis.h>
#endif

#include <sstream>

namespace Database
{

#ifdef DATABASE_HAS_REDIS

    RedisConnection::RedisConnection(const ConnectionConfig &config)
    { m_configuration = config; }

    RedisConnection::~RedisConnection() { disconnect(); }

    bool RedisConnection::connect()
    {
        if (m_isConnected) return true;
        struct timeval timeout;
        timeout.tv_sec  = m_connectTimeout / 1000;
        timeout.tv_usec = (m_connectTimeout % 1000) * 1000;

        m_redisContext = redisConnectWithTimeout(
            m_configuration.host.c_str(), m_configuration.port, timeout);
        if (!m_redisContext || m_redisContext->err) { captureError(); return false; }

        if (!m_configuration.password.empty())
        {
            auto *reply = static_cast<redisReply *>(
                redisCommand(m_redisContext, "AUTH %s", m_configuration.password.c_str()));
            if (!reply) { captureError(); return false; }
            if (reply->type == REDIS_REPLY_ERROR)
            {
                m_lastError = std::string(reply->str, reply->len);
                freeReplyObject(reply);
                redisFree(m_redisContext); m_redisContext = nullptr;
                return false;
            }
            freeReplyObject(reply);
        }
        m_isConnected = true; m_lastError.clear();
        return true;
    }

    void RedisConnection::disconnect()
    {
        if (!m_isConnected) return;
        if (m_redisContext) { redisFree(m_redisContext); m_redisContext = nullptr; }
        m_pipelineBuffer.clear(); m_pipelineMode = false; m_isConnected = false;
    }

    bool RedisConnection::isConnected() const
    { return m_isConnected && m_redisContext; }

    std::unique_ptr<DatabaseResult> RedisConnection::execute(std::string_view command)
    {
        if (!m_isConnected || !m_redisContext)
        { m_lastError = "未连接到 Redis"; return nullptr; }
        auto *reply = static_cast<redisReply *>(
            redisCommand(m_redisContext, "%.*s",
                         static_cast<int>(command.size()), command.data()));
        if (!reply) { captureError(); return nullptr; }
        return std::make_unique<RedisResult>(reply);
    }

    std::unique_ptr<DatabaseResult> RedisConnection::executeCommand(
        const std::vector<std::string_view> &arguments)
    {
        if (arguments.empty()) { m_lastError = "参数为空"; return nullptr; }
        std::ostringstream ss;
        for (size_t i = 0; i < arguments.size(); ++i)
        { if (i > 0) ss << ' '; ss << arguments[i]; }
        return execute(ss.str());
    }

    void RedisConnection::pipelineCommand(std::string_view command)
    { m_pipelineMode = true; m_pipelineBuffer.emplace_back(command); }

    std::vector<std::unique_ptr<DatabaseResult>> RedisConnection::flushPipeline()
    {
        std::vector<std::unique_ptr<DatabaseResult>> results;
        results.reserve(m_pipelineBuffer.size());
        if (!m_isConnected || !m_redisContext)
        { m_lastError = "未连接到 Redis"; return results; }

        for (const auto &cmd : m_pipelineBuffer)
            redisAppendCommand(m_redisContext, cmd.c_str());
        for (size_t i = 0; i < m_pipelineBuffer.size(); ++i)
        {
            redisReply *reply = nullptr;
            if (redisGetReply(m_redisContext, reinterpret_cast<void **>(&reply)) == REDIS_OK && reply)
                results.push_back(std::make_unique<RedisResult>(reply));
            else { captureError(); results.push_back(nullptr); }
        }
        m_pipelineBuffer.clear(); m_pipelineMode = false;
        return results;
    }

    bool RedisConnection::selectDatabase(int index)
    { auto r = execute("SELECT " + std::to_string(index)); return r && !r->isEmpty(); }

    DatabaseType RedisConnection::databaseType() const { return DatabaseType::Redis; }
    std::string RedisConnection::lastError() const { return m_lastError; }

    void RedisConnection::captureError()
    { m_lastError = m_redisContext ? m_redisContext->errstr : "Redis 未初始化"; }

#else // DATABASE_HAS_REDIS — 桩实现

    RedisConnection::RedisConnection(const ConnectionConfig &config)
    { m_configuration = config; m_lastError = "Redis 支持未编译（缺少 hiredis）"; }
    RedisConnection::~RedisConnection() = default;
    bool RedisConnection::connect() { return false; }
    void RedisConnection::disconnect() { m_isConnected = false; }
    bool RedisConnection::isConnected() const { return false; }
    std::unique_ptr<DatabaseResult> RedisConnection::execute(std::string_view) { return nullptr; }
    std::unique_ptr<DatabaseResult> RedisConnection::executeCommand(
        const std::vector<std::string_view> &) { return nullptr; }
    void RedisConnection::pipelineCommand(std::string_view) {}
    std::vector<std::unique_ptr<DatabaseResult>> RedisConnection::flushPipeline() { return {}; }
    bool RedisConnection::selectDatabase(int) { return false; }
    DatabaseType RedisConnection::databaseType() const { return DatabaseType::Redis; }
    std::string RedisConnection::lastError() const { return m_lastError; }
    void RedisConnection::captureError() {}

#endif

} // namespace Database
