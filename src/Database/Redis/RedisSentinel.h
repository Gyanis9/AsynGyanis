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
     * @brief 一次主库地址发现失败的类别
     * @details 按「调用方下一步做什么」切，不按本函数走到哪一步切：这四档在编排层是四种处置——
     *          改配置、改监控名、换时间与重试、或报「协议对不上」并停下来
     */
    enum class RedisSentinelErrorKind
    {
        InvalidConfiguration, ///< 传进来的地址表或监控名不成形：还没发出任何字节
        MasterNotMonitored,   ///< 哨兵明确回答「没有监控这个名字」：各哨兵之间是同步的，换下一个也一样
        NoSentinelReachable,  ///< 问遍了都没拿到答案：连不上或答不上，属可用性故障；地址表本身不完整走 InvalidConfiguration
        MalformedReply,       ///< 哨兵答了却不是约定的形状：版本对不上或被中间层改过，该停下来查
    };

    /**
     * @brief 一条发现失败：类别供分支，文案供人看
     */
    struct ASYN_DATABASE_API RedisSentinelError
    {
        RedisSentinelErrorKind kind;    ///< 失败类别，编排层据此选处置
        std::string            message; ///< 中文原因，含是哪一台哨兵、哪一步与回复现场
    };

    /**
     * @brief 发现的结果形状：成功带 Value，失败侧固定为 RedisSentinelError
     * @details 只有这一半有消费方：发现这一步要么给出一对地址要么给不出，没有「只问成不成」的调用面，
     *          因此不造没人用的 `RedisSentinelStatus`
     */
    template<typename Value>
    using RedisSentinelExpected = std::expected<Value, RedisSentinelError>;

    /**
     * @brief 向哨兵问一个被监控主库的当前地址（`SENTINEL get-master-addr-by-name`）
     *
     * @details 这是「主从失效转移」里只读的那一步：把一个监控名换成一对口前的主库地址，
     *          调用方拿着它去填自己的 ConnectionConfig、再建连接或连接池。本函数不做故障转移、
     *          不写任何哨兵配置、也不在失败时改用另一个主库——那些是编排策略，不属于驱动。
     *          逐个哨兵按给定顺序试，第一个答得出名字的赢：哨兵集群里各份配置是同步的，
     *          因此「名字不存在」与「这个哨兵答不上」是两件事，前者是配置错误、当场终止，
     *          后者才继续问下一个。
     *
     * @details 与连接配置的两处刻意差异要写清（都是实测出来的）：哨兵不接受 `SELECT`
     *          （实测回 `ERR unknown command 'SELECT'`），因此本函数把传入配置里的
     *          database 一格清掉再连——留着它会让每一次发现都在第一条命令上失败，而失败原因
     *          看起来像「哨兵坏了」；`sentinelCredentials` 里的口令是哨兵那边的凭据
     *          （多数部署没有），与主库的口令无关。
     *
     * @note 只做 Sentinel 模式，不做 Cluster（这是决定，不是待办：那是另一个子系统）。
     *       Cluster 的 `CLUSTER SLOTS` 那一族本引擎不实现，也不通告支持。
     *
     * @param sentinels 哨兵地址表，按尝试顺序给出；空表是配置错误
     * @param masterName `sentinel monitor` 里那个监控名
     * @param sentinelCredentials 连哨兵要用的配置（主机与端口按表覆盖；database 一格被忽略）
     * @return RedisSentinelExpected<RedisSentinelAddress> 当前主库地址
     * @return RedisSentinelError 带类别与中文原因：配置不合法是 `InvalidConfiguration`，监控名不被
     *         任何哨兵承认是 `MasterNotMonitored`，全部哨兵都问不到是 `NoSentinelReachable`，
     *         答了却不是约定形状是 `MalformedReply`。原因一律中文且点明是哪一个哨兵、哪一步
     */
    [[nodiscard]] ASYN_DATABASE_API RedisSentinelExpected<RedisSentinelAddress>
    resolveSentinelMasterAddress(std::span<const RedisSentinelAddress> sentinels, std::string_view masterName, const ConnectionConfig &sentinelCredentials);

} // namespace AsynGyanis::Database
