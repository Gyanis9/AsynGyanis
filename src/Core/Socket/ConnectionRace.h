/**
 * @file ConnectionRace.h
 * @brief 多候选地址的并发连接竞赛：同时向解析出的候选发起连接，第一个连上的赢，其余当场收口
 * @author Gyanis
 * @date 2026-09-26
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once

#include "Core/Coroutine/Task.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Socket/InetAddress.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;

    /**
     * @brief 一场连接竞赛的胜者
     */
    struct ConnectedCandidate
    {
        AsyncSocket socket;    ///< 连上的那条套接字（已建立，可直接交给上层）
        InetAddress address;   ///< 连上的是哪个候选地址：调用方要按它的协议族后续处理，日志也靠它定位
    };

    /**
     * @brief 把解析出来的候选地址排成「族间交错、族内保序」的连接顺序
     * @param resolved 解析器给出的候选地址（已按 RFC 6724 的偏好排好序）
     * @return std::vector<InetAddress> 交错后的顺序；空输入给出空输出
     * @details RFC 8305 §4 要求的顺序：先按 RFC 6724 §5 排序（本框架里由 getaddrinfo 完成），再把两族
     *          各自成列，按「首选族在前、两族交替」交错。交替的理由是**不让一族把整场占满**：一台双栈
     *          主机若把 IPv6 的几条排在前面而 IPv6 链路正在黑洞，纯按排序顺序试就永远轮不到 IPv4。
     *          首选族取排序结果第一条所属的那一族，也就是保留 RFC 6724 的偏好；一族排空后，另一族按
     *          原序接在后面。本函数不改集合、只改顺序，且对同一输入给出同一顺序（用例可逐位比对）。
     */
    [[nodiscard]] std::vector<InetAddress> orderForConnectionRace(const std::vector<InetAddress> &resolved);

    /**
     * @brief 同时在途的候选连接上限
     * @details 竞赛不排队、直接并发起，上界是必需的护栏：一台主机可能给出十几个 A/AAAA 记录，
     *          而连接池冷启动时同进程会有几十个端点一起解析。四条够覆盖「首选族黑洞 + 另一族可用」
     *          这一实际故障形状。排在这之后的候选不会漏：前面每收口一条就补发一条，而预算按
     *          「还要几轮」切均，所以它们一定在整场时限内拿到自己的那一轮。
     */
    inline constexpr std::size_t kMaximumConcurrentCandidates = 4U;

    /**
     * @brief 并发地向候选地址发起 TCP 连接：第一个连上的赢，其余立刻收口
     * @param loop 所属事件循环
     * @param candidates 候选地址（**按值接收**：本方法是惰性启动的协程，函数体到首次 resume 才执行，
     *        按引用接收会让调用方的临时数组先它一步销毁）；顺序原样采用，不做排序
     * @param deadline 整场竞赛的时限；每条候选拿到的那份按「后面还排着几轮」切均，因此整场不会超过
     *        这个数，而排在在途上限之后的候选也不会被前面的候选饿死
     * @return Task<std::optional<ConnectedCandidate>> 连上交出胜者，全部失败或时限到点交出空值
     * @details 为什么并发而不是按顺序一条条试（本框架此前的做法）：一条**黑洞**地址（SYN 发出去没人
     *          应答，常见于 IPv6 链路已断却仍路由得出去）要等到时限才收口，而顺序试法里这段时间
     *          是整场预算——第一条候选就能把预算吃干净，后面的候选根本轮不到，表现为「双栈主机上
     *          连不上、纯 IPv4 主机却连得上」。并发试法把这条地址压回它自己的那一段，与 RFC 8305
     *          的错峰发起（§5.4）目的一致：不让单个候选拖住整场。差别在于本函数用「同时在途上限」
     *          取代了 200 ms 的固定错峰：框架的定时器等待器没有「提前唤醒」接口，错峰等待一旦不能被
     *          收场信号打断，秒失败的常见路径（对端直接 RST）就要白等 200 ms。并发起三条的代价只是
     *          多两个 SYN 包与两次「连上即关」，对端看到的是寻常的探测式连接。
     * @note 失败的候选各留一条 WARN 日志（底层原文只进日志，不外传）；胜者之外连上的那条会被关掉，
     *       对端因此会看到一次建立又立刻结束的连接
     * @note 时限到点时**不会**留下半开的套接字：每条候选自己的看门狗到点就把它关掉，协程收口才计入
     *       全场结束，因此本函数返回时所有已发起的描述符都已归还
     */
    Task<std::optional<ConnectedCandidate>> connectCandidates(EventLoop &loop,
                                                             std::vector<InetAddress> candidates,
                                                             std::chrono::milliseconds deadline);
} // namespace AsynGyanis::Core
