/**
 * @file RedisSentinel.h
 * @brief Redis 哨兵（Sentinel）的主库地址发现
 * @author Gyanis
 * @date 2026-10-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once

#include "AsynGyanisExport.h"

#include "Database/Common/ConnectionConfig.h"

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace AsynGyanis::Database
{
    /**
     * @brief 一个哨兵或主库的网络地址
     *
     * @details host 与 port 分开给，不复用 ConnectionConfig：发现这一步要的只是一对地址，
     *          把口令、TLS 档位那些字段一起摊进来会让人以为哨兵的连接配置和主库的是同一份
     *          （它们通常不是——哨兵那边一般没有口令，主库那边有）。
     * @note port 为 0 表示未指定，发现路径会把它当成配置错误跳过并写明原因，
     *       不会退化成「连一个 0 号端口碰碰运气」。
     */
    struct ASYN_DATABASE_API RedisSentinelAddress
    {
        std::string   host;    ///< 主机地址或主机名
        std::uint16_t port{0}; ///< 端口号；哨兵默认 26379，0 表示未指定
    };

    /**
     * @brief 向哨兵问一个被监控主库的当前地址（`SENTINEL get-master-addr-by-name`）
     *
     * @details 这是「主从失效转移」里**只读的那一步**：把一个监控名换成一对口前的主库地址，
     *          调用方拿着它去填自己的 ConnectionConfig、再建连接或连接池。本函数不做故障转移、
     *          不写任何哨兵配置、也不在失败时改用另一个主库——那些是编排策略，不属于驱动。
     *          逐个哨兵按给定顺序试，第一个答得出名字的赢：哨兵集群里各份配置是同步的，
     *          因此「名字不存在」与「这个哨兵答不上」是两件事，前者是配置错误、当场终止，
     *          后者才继续问下一个。
     *
     * @details 与连接配置的两处刻意差异要写清（都是实测出来的）：哨兵**不接受** `SELECT`
     *          （实测回 `ERR unknown command 'SELECT'`），因此本函数把传入配置里的
     *          database 一格清掉再连——留着它会让每一次发现都在第一条命令上失败，而失败原因
     *          看起来像「哨兵坏了」；`sentinelCredentials` 里的口令是**哨兵那边**的凭据
     *          （多数部署没有），与主库的口令无关。
     *
     * @note 只做 Sentinel 模式，不做 Cluster（提示词判死：那是另一个子系统）。
     *       Cluster 的 `CLUSTER SLOTS` 那一族本引擎不实现，也不通告支持。
     *
     * @param sentinels 哨兵地址表，按尝试顺序给出；空表是配置错误
     * @param masterName `sentinel monitor` 里那个监控名
     * @param sentinelCredentials 连哨兵要用的配置（主机与端口按表覆盖；database 一格被忽略）
     * @return std::expected<RedisSentinelAddress, std::string> 当前主库地址
     * @return std::unexpected 配置不合法、监控名不被任何哨兵承认，或全部哨兵都问不到，
     *         原因是中文且点明是哪一个哨兵、哪一步
     */
    [[nodiscard]] ASYN_DATABASE_API std::expected<RedisSentinelAddress, std::string>
    resolveSentinelMasterAddress(std::span<const RedisSentinelAddress> sentinels, std::string_view masterName, const ConnectionConfig &sentinelCredentials);

} // namespace AsynGyanis::Database
