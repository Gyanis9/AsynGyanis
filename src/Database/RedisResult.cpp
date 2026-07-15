/**
 * @file RedisResult.cpp
 * @brief Redis 结果集实现
 * @copyright Copyright (c) 2026
 */

#include "RedisResult.h"

#ifdef DATABASE_HAS_REDIS
#include <hiredis/hiredis.h>
#endif

namespace Database
{

#ifdef DATABASE_HAS_REDIS

    RedisResult::RedisResult(redisReply *const reply) : m_replyPointer(reply)
    {
        if (!m_replyPointer) { m_isEmpty = true; return; }
        m_replyType     = m_replyPointer->type;
        m_totalElements = (m_replyType == REDIS_REPLY_ARRAY) ? m_replyPointer->elements : 1;
        m_isEmpty       = (m_replyType == REDIS_REPLY_NIL);
    }

    RedisResult::~RedisResult()
    {
        if (m_replyPointer) freeReplyObject(m_replyPointer);
    }

    bool RedisResult::next()
    {
        if (m_currentIndex >= m_totalElements) return false;
        ++m_currentIndex;
        return m_currentIndex <= m_totalElements;
    }

    size_t RedisResult::rowCount() const { return m_totalElements; }
    size_t RedisResult::columnCount() const { return 1; }

    std::optional<std::string> RedisResult::columnName(size_t) const
    { return m_totalElements > 0 ? std::optional(std::string("value")) : std::nullopt; }

    std::optional<size_t> RedisResult::columnIndex(std::string_view name) const
    { return (name == "value" && m_totalElements > 0) ? std::optional<size_t>(0) : std::nullopt; }

    DatabaseValue RedisResult::getValue(const size_t index) const
    {
        if (!m_replyPointer) return std::monostate{};
        if (m_replyType == REDIS_REPLY_ARRAY)
            return (index < m_replyPointer->elements)
                ? convertReply(m_replyPointer->element[index]) : std::monostate{};
        return (index == 0) ? convertReply(m_replyPointer) : std::monostate{};
    }

    DatabaseValue RedisResult::getValue(std::string_view name) const
    { return (name == "value") ? getValue(0) : std::monostate{}; }

    std::vector<std::string> RedisResult::columnNames() const
    { return m_totalElements > 0 ? std::vector<std::string>{"value"} : std::vector<std::string>{}; }

    void RedisResult::reset() { m_currentIndex = 0; }
    bool RedisResult::isEmpty() const { return m_isEmpty; }
    bool RedisResult::isError() const { return m_replyType == REDIS_REPLY_ERROR; }

    DatabaseValue RedisResult::convertReply(const redisReply *reply) const
    {
        if (!reply) return std::monostate{};
        switch (reply->type)
        {
            case REDIS_REPLY_STRING:  return std::string(reply->str, reply->len);
            case REDIS_REPLY_INTEGER: return static_cast<int64_t>(reply->integer);
            case REDIS_REPLY_NIL:     return std::monostate{};
            case REDIS_REPLY_STATUS:  return std::string(reply->str, reply->len);
            case REDIS_REPLY_ARRAY:
            {
                std::vector<std::string> list; list.reserve(reply->elements);
                for (size_t i = 0; i < reply->elements; ++i)
                {
                    auto val = convertReply(reply->element[i]);
                    if (std::holds_alternative<std::string>(val))
                        list.push_back(std::get<std::string>(val));
                }
                return list;
            }
            case REDIS_REPLY_ERROR:
                m_lastError = std::string(reply->str, reply->len);
                return std::monostate{};
            default: return std::monostate{};
        }
    }

#else // DATABASE_HAS_REDIS — 桩实现

    RedisResult::RedisResult(redisReply *) { m_isEmpty = true; }
    RedisResult::~RedisResult() = default;
    bool RedisResult::next() { return false; }
    size_t RedisResult::rowCount() const { return 0; }
    size_t RedisResult::columnCount() const { return 0; }
    std::optional<std::string> RedisResult::columnName(size_t) const { return std::nullopt; }
    std::optional<size_t> RedisResult::columnIndex(std::string_view) const { return std::nullopt; }
    DatabaseValue RedisResult::getValue(size_t) const { return std::monostate{}; }
    DatabaseValue RedisResult::getValue(std::string_view) const { return std::monostate{}; }
    std::vector<std::string> RedisResult::columnNames() const { return {}; }
    void RedisResult::reset() {}
    bool RedisResult::isEmpty() const { return true; }
    bool RedisResult::isError() const { return false; }
    DatabaseValue RedisResult::convertReply(const redisReply *) const { return std::monostate{}; }

#endif

} // namespace Database
