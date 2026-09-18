/**
 * @file RedisReplyText.h
 * @brief hiredis 回复节点的文本摘取 —— Redis 连接与结果集共用（仅驱动实现可包含）
 * @author Gyanis
 * @date 2026-09-18
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 本头包含 <hiredis/hiredis.h>，只允许被 Redis 驱动的 .cpp 在
 *          `#ifdef DATABASE_HAS_REDIS` 区内包含；对外的前置声明仍在 RedisConnection.h。
 */

#pragma once

#include <hiredis/hiredis.h>

#include <string>

namespace AsynGyanis::Database
{
    /**
     * @brief 按长度摘取回复节点携带的原始文本
     * @param sourceReply 回复节点，可为 nullptr
     * @return std::string 文本副本；节点为空或没有文本域时返回空串
     */
    [[nodiscard]] inline std::string copyReplyText(const redisReply *sourceReply)
    {
        // hiredis 用 str + len 表达文本，不保证零终止且可以内嵌 '\0'，必须按长度拷贝
        if (sourceReply == nullptr || sourceReply->str == nullptr)
        {
            return {};
        }
        return {sourceReply->str, sourceReply->len};
    }
} // namespace AsynGyanis::Database
