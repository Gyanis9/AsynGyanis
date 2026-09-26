/**
 * @file AsyncResolver.h
 * @brief 异步 DNS 解析器 —— 在后台线程执行 getaddrinfo，结果投回事件循环
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Core/Socket/InetAddress.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;

    /**
     * @brief 异步 DNS 主机名解析器
     *
     * @details 将阻塞的 getaddrinfo 调用卸到后台线程执行，结果经 Scheduler::postRemote
     *          投回调用方的事件循环，不会阻塞任何工作线程的事件循环。
     * @note 名字查询带一层进程级缓存（按「主机+端口」为键，默认 60 秒过期）：池冷启动、连接被对端
     *       收掉后重连、以及每一条新出站请求都要问一次同一个域名，不缓存就是每次一趟线程往返。
     *       只缓存**非空**结果——解析失败与「这个域名确实没有记录」在 getaddrinfo 的返回里是同一个
     *       空列表，把失败缓存 60 秒等于把一次抖动放大成一分钟连不上。IP 字面量不进这套流程
     *       （见 resolve 的说明），也就谈不上缓存。
     */
    class AsyncResolver
    {
    public:
        /**
         * @brief 解析器的观测读数（进程级累计）
         * @details 用「前后两次读数的差」来判，别读绝对值：这是进程级计数，同一进程里跑过的其它
         *          查询会一起算进来。
         */
        struct Stats
        {
            std::uint64_t lookupCount{0};    ///< 走进名字解析流程的查询次数；字面量直接构造地址，不算查询
            std::uint64_t cacheHitCount{0};  ///< 其中由缓存直接答出的次数
        };

        AsyncResolver() = default;

        AsyncResolver(const AsyncResolver &) = delete;

        AsyncResolver &operator=(const AsyncResolver &) = delete;

        /**
         * @brief 异步解析主机名（或 IP 字符串）及端口，返回所有可用地址
         * @param loop 发起方的事件循环（结果也投回这里）
         * @param host 主机名；空字符串返回空列表
         * @param port 端口号（主机字节序）
         * @return 按地址族分组的地址列表（IPv4 在前、IPv6 在后）；空列表表示解析失败或主机名为空
         * @note IP 字面量走**不经 getaddrinfo** 的直接构造：一是不必问任何人，二是 hints 里的
         *       AI_ADDRCONFIG 会按「本机有没有配到该族的非回环地址」筛结果——只有 ::1 可用的容器上
         *       连 getaddrinfo("::1") 都会报 EAI_ADDRFAMILY，写在脸上的地址反倒解析不出来。
         *       名字查询仍带那一项，那本来就是它的用途
         * @note getaddrinfo 是阻塞调用，卸到后台线程执行；返回时已回到发起协程的事件循环上
         * @note host 按值取 std::string 而不是视图：本函数是惰性 Task，帧体要等 co_await 才跑，
         *       视图参数在那时早已悬垂
         * @warning 解析线程是分离线程，本类没有「等它跑完」的收口点：它投回前会先看等待中的帧还在不在
         *       （不在就不碰循环），因此调用方的约束是普通的「销毁循环前先销毁挂在它上面的帧」。
         *       循环若会在有在途解析时被销毁，先把那些协程的帧收掉
         */
        static Task<std::vector<InetAddress>> resolve(EventLoop &loop, std::string host, uint16_t port);

        /**
         * @brief 取进程级的解析读数
         * @return Stats 累计的查询次数与其中命中缓存的次数
         * @note 用「前后两次读数的差」来判，别读绝对值：这是进程级计数，同一进程里跑过的其它用例会
         *       一起算进来
         */
        [[nodiscard]] static Stats stats() noexcept;
    };
} // namespace AsynGyanis::Core