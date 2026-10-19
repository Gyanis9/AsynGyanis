/**
 * @file RedisSentinel.cpp
 * @brief Redis 哨兵的主库地址发现实现
 * @author Gyanis
 * @date 2026-10-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#include "Database/Redis/RedisSentinel.h"

#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Redis/RedisConnection.h"

#include <charconv>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace AsynGyanis::Database
{
    namespace
    {
        /**
         * @brief 把一个哨兵地址折成文案里读得出的样子
         * @details 只报主机名分不出端口写错的那一档（哨兵默认 26379，很多人手滑填成 6379），
         *          因此主机与端口成对出现；主机为空时也不能交回一个 ":0" 这种看不懂的串
         * @param address 待描述的地址
         * @return std::string 形如 "127.0.0.1:26379" 的文本
         */
        [[nodiscard]] std::string describeAddress(const RedisSentinelAddress &address)
        {
            if (address.host.empty())
            {
                return "<主机名为空>";
            }
            return address.host + ":" + std::to_string(address.port);
        }
    } // namespace

    RedisSentinelExpected<RedisSentinelAddress> resolveSentinelMasterAddress(const std::span<const RedisSentinelAddress> sentinels, const std::string_view masterName,
                                                                             const ConnectionConfig &sentinelCredentials)
    {
        // 两处配置错误在发出任何字节之前就判掉：空表没有「下一个哨兵」可试，
        // 而空监控名会让每个哨兵都回一句「没有这个名字」，把一个填错的键说成服务端的问题
        if (sentinels.empty())
        {
            return std::unexpected(RedisSentinelError{RedisSentinelErrorKind::InvalidConfiguration, "没有给哨兵地址：地址表为空，无从问起主库在哪里"});
        }
        if (masterName.empty())
        {
            return std::unexpected(
                    RedisSentinelError{RedisSentinelErrorKind::InvalidConfiguration, "没有给主库监控名：SENTINEL get-master-addr-by-name 要的是 sentinel monitor 配置里那个名字"});
        }

        const std::string requestedName(masterName);
        std::string       failures;
        // 汇总那一档到底算「可用性故障」还是「配置错」，看有没有真碰过线：地址不完整的格子只是被跳过，
        // 连不上与答不上才是哨兵那边的问题——两者的处置相反（改配置 vs 换时间重试），不能混成一档
        bool sawReachabilityFailure = false;

        for (const RedisSentinelAddress &sentinel: sentinels)
        {
            if (sentinel.host.empty() || sentinel.port == 0)
            {
                failures += "；" + describeAddress(sentinel) + " 的地址不完整（主机名为空或端口未指定）";
                continue;
            }

            ConnectionConfig configuration = sentinelCredentials;
            configuration.host             = sentinel.host;
            configuration.port             = sentinel.port;
            // 哨兵不接受 SELECT（本机实测回 `ERR unknown command 'SELECT'`），因此这一格必须清掉：
            // 留着它会让每一次发现都在建连的最后一步失败，而原因看起来像「这个哨兵坏了」
            configuration.database.clear();

            RedisConnection connection(configuration);
            if (!connection.connect())
            {
                failures += "；" + describeAddress(sentinel) + " 连不上：" + connection.lastError();
                sawReachabilityFailure = true;
                continue;
            }

            const std::unique_ptr<DatabaseResult> reply = connection.executeCommand({std::string_view("SENTINEL"), std::string_view("get-master-addr-by-name"), masterName});
            if (reply == nullptr)
            {
                failures += "；" + describeAddress(sentinel) + " 问不到：" + connection.lastError();
                sawReachabilityFailure = true;
                continue;
            }

            // 空回复是「这个哨兵没有监控这个名字」。各哨兵的配置由哨兵集群自己同步，因此换一台
            // 也问不出同样的答案——这是调用方填错了名字，当场终止而不是把整张表跑一遍
            if (reply->isEmpty())
            {
                return std::unexpected(RedisSentinelError{RedisSentinelErrorKind::MasterNotMonitored,
                                                          "哨兵 " + describeAddress(sentinel) + " 没有监控名为「" + requestedName +
                                                                  "」的主库；这个名字在各哨兵之间是同步的，换下一个也问不出来，请核对 sentinel monitor 的配置"});
            }

            if (reply->columnCount() < 2)
            {
                return std::unexpected(
                        RedisSentinelError{RedisSentinelErrorKind::MalformedReply,
                                           "哨兵 " + describeAddress(sentinel) + " 给的主库地址不是「主机 + 端口」两项，实际读到 " + std::to_string(reply->columnCount()) + " 列"});
            }

            const DatabaseValue hostValue = reply->getValue(std::size_t{0});
            const DatabaseValue portValue = reply->getValue(std::size_t{1});
            const auto         *hostText  = std::get_if<std::string>(&hostValue);
            const auto         *portText  = std::get_if<std::string>(&portValue);
            if (hostText == nullptr || hostText->empty() || portText == nullptr)
            {
                return std::unexpected(RedisSentinelError{RedisSentinelErrorKind::MalformedReply,
                                                          "哨兵 " + describeAddress(sentinel) + " 给的主库地址读不出文本，回复形状与 SENTINEL get-master-addr-by-name 的约定不符"});
            }

            // 端口按文本给（本机实测 "6379"），因此自己解析而不是交给某个数字解析器的默认宽容：
            // 留余文（"6379abc"）与零都算读不懂，宁可报错也不连一个猜出来的端口
            unsigned int parsedPort     = 0;
            const char  *parseBegin     = portText->data();
            const char  *parseEnd       = parseBegin + portText->size();
            const auto   parseResult    = std::from_chars(parseBegin, parseEnd, parsedPort);
            const bool   isWholeDecimal = parseResult.ec == std::errc() && parseResult.ptr == parseEnd;
            if (!isWholeDecimal || parsedPort == 0 || parsedPort > 65535)
            {
                return std::unexpected(
                        RedisSentinelError{RedisSentinelErrorKind::MalformedReply, "哨兵 " + describeAddress(sentinel) + " 给的主库端口不是 1..65535 的十进制数：" + *portText});
            }

            return RedisSentinelAddress{*hostText, static_cast<std::uint16_t>(parsedPort)};
        }

        // 一格都没碰上线（表里全是跳过去的 incomplete 地址）时这是配置错，不是可用性故障——报成后者
        // 会把人支去查哨兵进程，而问题在自己的地址表里
        const RedisSentinelErrorKind aggregateKind = sawReachabilityFailure ? RedisSentinelErrorKind::NoSentinelReachable : RedisSentinelErrorKind::InvalidConfiguration;
        return std::unexpected(RedisSentinelError{aggregateKind, "问遍 " + std::to_string(sentinels.size()) + " 个哨兵都没拿到主库「" + requestedName + "」的地址" + failures});
    }

} // namespace AsynGyanis::Database
