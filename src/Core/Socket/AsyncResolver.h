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
#include <string_view>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;

    /**
     * @brief 异步 DNS 主机名解析器
     *
     * @details 将阻塞的 getaddrinfo 调用卸到后台线程执行，结果经 Scheduler::postRemote
     *          投回调用方的事件循环，不会阻塞任何工作线程的事件循环。
     */
    class AsyncResolver
    {
    public:
        AsyncResolver() = default;

        AsyncResolver(const AsyncResolver &) = delete;

        AsyncResolver &operator=(const AsyncResolver &) = delete;

        /**
         * @brief 异步解析主机名（或 IP 字符串）及端口，返回所有可用地址
         * @details 解析在当前协程的事件循环上发起，getaddrinfo 在后台线程执行；
         *          返回时已回到发起协程的事件循环上。
         * @param loop 发起方的事件循环（同时也是结果投递目标）
         * @param host 主机名；空字符串返回空列表
         * @param port 端口号（主机字节序）
         * @return Task<std::vector<InetAddress>> 按地址族分组的地址列表（IPv4 在前、IPv6 在后），
         *         空列表表示解析失败或主机名为空
         */
        static Task<std::vector<InetAddress>> resolve(EventLoop &loop, std::string_view host, uint16_t port);
    };
} // namespace AsynGyanis::Core