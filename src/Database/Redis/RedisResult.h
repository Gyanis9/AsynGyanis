/**
 * @file RedisResult.h
 * @brief Redis 命令结果封装
 * @copyright Copyright (c) 2026
 */

#ifndef DATABASE_REDISRESULT_H
#define DATABASE_REDISRESULT_H

#include "DatabaseResult.h"

// 前向声明 hiredis 结构体（实际使用时需 #include <hiredis/hiredis.h>）
struct redisReply;

namespace Database
{
    /**
     * @brief Redis 命令结果集
     *
     * 封装 hiredis 的 redisReply，将 Redis 的多种返回类型
     * （字符串、整数、数组、错误、状态等）统一映射为 DatabaseResult 接口。
     */
    class RedisResult : public DatabaseResult
    {
    public:
        /**
         * @brief 使用 hiredis 的 redisReply 构造
         * @param reply hiredis 返回的 redisReply 指针（接管所有权）
         */
        explicit RedisResult(redisReply *reply);

        /**
         * @brief 析构时自动释放 redisReply
         */
        ~RedisResult() override;

        RedisResult(const RedisResult &)            = delete;
        RedisResult &operator=(const RedisResult &) = delete;
        RedisResult(RedisResult &&)                 = default;
        RedisResult &operator=(RedisResult &&)      = default;

        bool next() override;
        [[nodiscard]] size_t rowCount() const override;
        [[nodiscard]] size_t columnCount() const override;
        [[nodiscard]] std::optional<std::string> columnName(size_t index) const override;
        [[nodiscard]] std::optional<size_t> columnIndex(std::string_view name) const override;
        [[nodiscard]] DatabaseValue getValue(size_t index) const override;
        [[nodiscard]] DatabaseValue getValue(std::string_view name) const override;
        [[nodiscard]] std::vector<std::string> columnNames() const override;
        void reset() override;
        [[nodiscard]] bool isEmpty() const override;

        /**
         * @brief 判断当前 Redis 回复是否为错误类型
         * @return 是错误返回 true
         */
        [[nodiscard]] bool isError() const;

        /**
         * @brief 获取 Redis 回复的类型标识
         * @return 类型标识（REDIS_REPLY_STRING / REDIS_REPLY_ARRAY 等）
         */
        [[nodiscard]] int replyType() const { return m_replyType; }

        /**
         * @brief 获取底层 redisReply 指针（仅供内部使用）
         */
        [[nodiscard]] const redisReply *nativeHandle() const { return m_replyPointer; }

    private:
        /**
         * @brief 将 redisReply 转换为统一的 DatabaseValue
         * @param reply hiredis 回复结构
         * @return 统一的数据库值
         */
        [[nodiscard]] DatabaseValue convertReply(const redisReply *reply) const;

        /**
         * @brief 递归释放 redisReply 及其子元素
         * @param reply 待释放的回复
         */
        static void freeReply(redisReply *reply);

        redisReply *m_replyPointer{nullptr}; ///< hiredis 回复结构指针
        int         m_replyType{0};          ///< 回复类型
        size_t      m_totalElements{0};      ///< 总元素数（数组类型时有效）
        size_t      m_currentIndex{0};       ///< 当前遍历索引
        bool        m_isEmpty{true};         ///< 是否为空结果集
    };

} // namespace Database

#endif // DATABASE_REDISRESULT_H
