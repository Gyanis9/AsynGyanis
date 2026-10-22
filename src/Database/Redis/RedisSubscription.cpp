/**
 * @file RedisSubscription.cpp
 * @brief Redis 订阅、退订与推送读取入口
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details SUBSCRIBE/PSUBSCRIBE 的确认收取、UNSUBSCRIBE/PUNSUBSCRIBE 的逐目标收尾、
 *          readPushReply 的推送解析与 RESP3 拒绝口径都在这里；建连、命令执行与会话清理
 *          留在 RedisConnection.cpp。
 */

#include "Database/Redis/RedisConnection.h"
#include "Database/Redis/RedisConnectionSupport.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifdef DATABASE_HAS_REDIS
#include <cerrno>
#include <hiredis/hiredis.h>
#endif

namespace AsynGyanis::Database
{
#ifdef DATABASE_HAS_REDIS
    using Detail::makeTimeval;

    namespace
    {
        /**
         * @brief 取推送回复数组里的第 index 个元素，按字符串读出
         * @param reply 服务端回的那条数组回复
         * @param index 下标
         * @return std::string 元素的字节内容；下标越界或该元素不带字符串时为空串
         * @note 元素访问按 elements / element[] 这一对名字走，理由见本文件里 RedisResult 的同款注释：
         *       各 hiredis 版本把数组暴露成什么并不一致
         */
        std::string pushReplyField(const redisReply &reply, const std::size_t index)
        {
            if (index >= reply.elements || reply.element == nullptr)
            {
                return {};
            }
            const redisReply *const element = reply.element[index];
            if (element == nullptr || element->str == nullptr)
            {
                return {};
            }
            return std::string(element->str, element->len);
        }

        /**
         * @brief 取订阅类回复末尾那个「当前订阅数」
         * @param reply 服务端回的那条数组回复
         * @param index 计数所在的元素下标
         * @return std::int64_t 计数；该元素不带整数时给 0（不猜）
         */
        std::int64_t pushReplyCount(const redisReply &reply, const std::size_t index)
        {
            if (index >= reply.elements || reply.element == nullptr)
            {
                return 0;
            }
            const redisReply *const element = reply.element[index];
            if (element == nullptr || element->type != REDIS_REPLY_INTEGER)
            {
                return 0;
            }
            return element->integer;
        }

        /**
         * @brief 把一条推送回复转成 RedisPushReply（不接管 reply 的所有权）
         * @param reply 服务端回的数组回复
         * @return RedisPushReply 按 kind 填好的字段
         */
        RedisPushReply convertPushReply(const redisReply &reply)
        {
            const std::string kind = pushReplyField(reply, 0);

            RedisPushReply push;
            push.kind = kind;
            if (kind == "pmessage")
            {
                push.pattern = pushReplyField(reply, 1);
                push.channel = pushReplyField(reply, 2);
                push.payload = pushReplyField(reply, 3);
            } else if (kind == "message" || kind == "smessage")
            {
                push.channel = pushReplyField(reply, 1);
                push.payload = pushReplyField(reply, 2);
            } else
            {
                // subscribe / unsubscribe / psubscribe / punsubscribe，以及本机制还不认识的 kind：
                // 一律按「第 2 段是目标、第 3 段是计数」的形状读，认不出的字段留空而不报错——
                // 服务端将来加新形态时调用方仍然看得见它，比在这里静默丢掉好查
                push.channel           = pushReplyField(reply, 1);
                push.subscriptionCount = pushReplyCount(reply, 2);
            }
            return push;
        }

        /// 收订阅/退订确认时的单次等待上限：确认紧随命令到达，等满这么久就是没来（按失败收场）
        constexpr std::chrono::milliseconds kConfirmationWait{5000};

        /**
         * @brief RESP3 档下「靠推送送达」的那几条入口统一的拒绝文案
         * @details 为什么必须拒而不是「发了再说」：RESP3 把订阅确认与后续消息都写成 `>` 推送
         *          （本机 Redis 8.10.1 实测：`SUBSCRIBE ch` 的回包是 `>3 subscribe ch 1`，
         *          `MONITOR` 之后每条监视行同样是推送），而 hiredis 在没有装推送回调时会在
         *          `redisGetReply` 内部把推送就地释放并继续等一条「真正的回复」——订阅那条命令因此
         *          永远等不到东西，实测是等满 30 秒读写超时后报 `recv timeout`。
         *          把这条挂死伪装成「探针式地试一下」比直接拒更坏：调用方看到的是超时错误码，
         *          看不出档位与协议模型不相容这件事。要支持它需要的是推送回调 + 「发出订阅后不再
         *          等命令回复」的那套改法，属另一件事
         */
        constexpr std::string_view kResp3PushPathRefusal = "这条命令在 RESP3 档下不可用：订阅确认与服务端推送在 RESP3 里走 `>` 推送，而本驱动按「一条命令一条回复」执行，"
                                                           "hiredis 未接推送回调时这类回复会被就地丢弃、命令永远等不到回答。请把 redisProtocol 留在 Resp2 用它，"
                                                           "或改用 RESP3 仍能正常应答的命令（HGETALL / SMEMBERS / INFO 那一族）";
    } // namespace

    bool RedisConnection::subscribe(const std::span<const std::string_view> channels)
    {
        return startSubscription("SUBSCRIBE", channels);
    }

    bool RedisConnection::psubscribe(const std::span<const std::string_view> patterns)
    {
        return startSubscription("PSUBSCRIBE", patterns);
    }

    bool RedisConnection::startSubscription(const std::string_view commandName, const std::span<const std::string_view> targets)
    {
        m_lastError.clear();

        if (targets.empty())
        {
            // 空目标不该发出去：服务端会回「ERR wrong number of arguments」，而那看着像本类没接好命令。
            // 更要紧的是发出过一条订阅命令就会把连接标成推送形态，白丢一条可复用的连接
            m_lastError = "Redis 订阅命令没有目标：SUBSCRIBE 要至少一个频道，PSUBSCRIBE 要至少一个模式";
            return false;
        }

        // 「现在真正阻塞的是什么」排在档位判据之前：没连上时报未连接，比报档位更贴近现场
        if (!isConnected())
        {
            m_lastError = "未连接到 Redis，命令未执行";
            return false;
        }

        // RESP3 档的订阅确认是推送，本模型收不到（原因与取证见 kResp3PushPathRefusal）。
        // 在发送之前就拒，因此这条连接也不会被顺手记成「已进推送形态」——拒掉的订阅不该弄脏还能用的连接
        if (m_configuration.redisProtocol == DatabaseRedisProtocol::Resp3)
        {
            m_lastError = std::string(kResp3PushPathRefusal);
            return false;
        }

        std::vector<std::string_view> argumentValues;
        argumentValues.reserve(targets.size() + 1);
        argumentValues.push_back(commandName);
        argumentValues.insert(argumentValues.end(), targets.begin(), targets.end());

        // 发送与「服务端认了这条命令」的判据都复用同一条路径：它顺手把 m_isSessionModeChanged 记上
        // （订阅是退不回去的会话状态，见 noteSessionCommand），归还时池会断开这条连接
        if (executeArguments(argumentValues) == nullptr)
        {
            return false;
        }

        // 按类记条数：unsubscribeAll() 要知道每条退订命令该收几确认（回复里那个整数是两类合计，
        // 单看它分不出「这一类退干净了没」）
        // 名单按去重记：服务端为一条订阅只持一份，无参数 UNSUBSCRIBE 就按这份名单回确认。
        // 下面那条「补收 targets.size() - 1 条」仍然按目标数走——本机实测 `SUBSCRIBE a a` 确实为
        // 每个参数各回一条 subscribe 确认（计数都写 1），要分开的只是「这一条命令回几条」与
        // 「这条连接上真有几份订阅」这两个问题
        std::set<std::string> &subscriptions = commandName == "PSUBSCRIBE" ? m_patternSubscriptions : m_channelSubscriptions;
        for (const std::string_view target: targets)
        {
            subscriptions.insert(std::string(target));
        }

        // 服务端为每个目标各回一条确认。第一条已经作为这条命令的回复被 executeArguments 取走了
        // （那一刻连接还没进入推送形态，配对是成立的），因此这里只补收剩下的 targets.size() - 1 条。
        // 多等一条会一直等到超时——服务端根本没有那一条；少等一条则把确认留在流里，
        // 下一次 readPushReply() 就会把「subscribe 确认」当成消息交给调用方
        for (std::size_t index = 1; index < targets.size(); ++index)
        {
            if (!readPushReply(kConfirmationWait).has_value())
            {
                if (m_lastError.empty())
                {
                    // 超时不是失败路径给的错误，这里补一句：确认没收齐就是订阅状态不确定
                    m_lastError = "没有等齐服务端为这条订阅命令回的确认，订阅状态不确定";
                }
                return false;
            }
        }
        return true;
    }

    bool RedisConnection::unsubscribeAll()
    {
        m_lastError.clear();

        if (!isSubscribing())
        {
            // 本来就没订阅：什么都不发。发 UNSUBSCRIBE 会在服务端那边回一条确认，
            // 白付一次往返，而调用方要的是「退干净了」这个结论
            return true;
        }

        // 两类订阅各自退，各按自己那份名单的长度收确认。带错一条就会把另一类的确认吃掉，
        // 因此先退干净的先清零，不让两类共用一个计数
        for (const std::pair<std::string_view, std::set<std::string> *> &command:
             {std::pair{std::string_view{"UNSUBSCRIBE"}, &m_channelSubscriptions}, std::pair{std::string_view{"PUNSUBSCRIBE"}, &m_patternSubscriptions}})
        {
            const std::size_t outstanding = command.second->size();
            if (outstanding == 0)
            {
                continue; // 这一类本来就没有订阅：不发命令，也就没有要收的确认
            }

            const std::array<std::string_view, 1> arguments{command.first};
            if (executeArguments(arguments) == nullptr)
            {
                // 确认没齐：名单必须保留，isSubscribing() 才能如实说「状态不确定」；
                // 清空会让调用方以为已经退干净，而服务端可能仍在推送模式
                return false;
            }

            // 同一条命令的回复路径已经收走第一条确认，剩下 outstanding - 1 条在这里收干
            for (std::size_t index = 1; index < outstanding; ++index)
            {
                if (!readPushReply(kConfirmationWait).has_value())
                {
                    if (m_lastError.empty())
                    {
                        m_lastError = "没有等齐服务端为退订命令回的确认，订阅状态不确定：请按断开重连处理";
                    }
                    // 确认没齐：名单必须保留，isSubscribing() 才能如实说「状态不确定」；
                    // 清空会让调用方以为已经退干净，而服务端可能仍在推送模式
                    return false;
                }
            }
            command.second->clear();
        }

        // 订阅全退了，但 m_isSessionModeChanged 保持原样：这条连接确实经历过推送形态，
        // 让池按既有判据把它换掉比赌「服务端与我这边状态一致」便宜，判据只有一处
        return true;
    }

    std::optional<RedisPushReply> RedisConnection::readPushReply(const std::chrono::milliseconds waitTimeout)
    {
        m_lastError.clear();

        if (m_redisContext == nullptr)
        {
            m_lastError = "未连接到 Redis，读不到推送";
            return std::nullopt;
        }

        // 同一条档位的理由：RESP3 的推送被 hiredis 就地丢弃，这里等下去只会等满本次时限然后报
        // 「没等到消息」——那把「档位不支持」伪装成「对端没发」，排查方向整个反了
        if (m_configuration.redisProtocol == DatabaseRedisProtocol::Resp3)
        {
            m_lastError = std::string(kResp3PushPathRefusal);
            return std::nullopt;
        }

        // 本次等待上限临时盖在连接的 queryTimeout 之上：订阅消费的节奏归调用方定，
        // 读完必须还原——留着这个小值会把下一条普通命令提前判成超时
        const bool usesOwnWait = waitTimeout.count() > 0;
        if (usesOwnWait)
        {
            const struct timeval ownTimeout = makeTimeval(waitTimeout.count());
            if (redisSetTimeout(m_redisContext, ownTimeout) != REDIS_OK)
            {
                captureError("设置 Redis 推送等待时限失败");
                if (!applyQueryTimeout())
                {
                    disconnect();
                }
                return std::nullopt;
            }
        }

        void     *rawReplyPointer = nullptr;
        const int readStatus      = redisGetReply(m_redisContext, &rawReplyPointer);
        const int nativeErrno     = errno; // 紧接着就取：后面任何一次调用都会把它冲掉

        // 「这次没等到消息」的判据。hiredis 1.1 起读超时是 REDIS_ERR_TIMEOUT（本机 hiredis 1.3.0
        // 实测 err==6、errstr=="recv timeout"）；更早的版本走 REDIS_ERR_IO + errno，一并认下。
        // 认不出的一律按真失败处理：宁可多断开一次，也不要把一条已经坏了的上下文当成「只是没消息」继续等
        const bool timedOut = m_redisContext->err == REDIS_ERR_TIMEOUT ||
                              (m_redisContext->err == REDIS_ERR_IO && (nativeErrno == EAGAIN || nativeErrno == EWOULDBLOCK || nativeErrno == ETIMEDOUT));

        if (usesOwnWait && !applyQueryTimeout())
        {
            // 还原不上就说不清下一条普通命令会用哪个超时：reply 先释放，连接断开，
            // 不能把一次成功的 push 和一条非空的 lastError 一起交出去
            if (rawReplyPointer != nullptr)
            {
                freeReplyObject(rawReplyPointer);
            }
            disconnect();
            return std::nullopt;
        }

        if (readStatus != REDIS_OK || rawReplyPointer == nullptr)
        {
            // hiredis 在失败时也可能交出半截回复，非空就得释放，否则直接泄漏——与上面读管道回复
            // 那一路（flushPipeline）同一条规矩，所有权在这一格始终归本函数，没人接手就必须自己还
            if (rawReplyPointer != nullptr)
            {
                freeReplyObject(rawReplyPointer);
            }

            if (timedOut)
            {
                // 等不到消息是持续消费的正常节奏，不是失败：hiredis 把超时报成上下文错误，
                // 但套接字与读缓冲都没坏，清掉错误标记就能接着读下一条
                m_redisContext->err       = 0;
                m_redisContext->errstr[0] = '\0';
                return std::nullopt;
            }
            captureError("读取 Redis 推送失败");
            disconnect();
            return std::nullopt;
        }

        auto *const reply = static_cast<redisReply *>(rawReplyPointer);
        // 所有权在这条路径上一直归本函数：既没交给 RedisResult 也没被连接持有，读完必须自己释放
        const RedisPushReply push = convertPushReply(*reply);
        freeReplyObject(reply);
        return push;
    }

#else // DATABASE_HAS_REDIS —— 桩实现：订阅入口同样明确失败

    namespace
    {
        constexpr const char *kMissingDriverError = "当前构建未编译 Redis 驱动（缺少 hiredis）";
    } // namespace

    bool RedisConnection::subscribe(const std::span<const std::string_view>)
    {
        m_lastError = kMissingDriverError;
        return false;
    }

    bool RedisConnection::psubscribe(const std::span<const std::string_view>)
    {
        m_lastError = kMissingDriverError;
        return false;
    }

    bool RedisConnection::unsubscribeAll()
    {
        m_lastError = kMissingDriverError;
        return false;
    }

    std::optional<RedisPushReply> RedisConnection::readPushReply(const std::chrono::milliseconds)
    {
        m_lastError = kMissingDriverError;
        return std::nullopt;
    }

#endif // DATABASE_HAS_REDIS
} // namespace AsynGyanis::Database
