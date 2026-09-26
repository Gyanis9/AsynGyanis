/**
 * @file TcpClient.h
 * @brief 出站 TCP 客户端 —— 主机名解析 + 异步连接 + 自动回落地址族
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Net/Tcp/TcpStream.h"

#include <cstdint>
#include <memory>
#include <string>

namespace AsynGyanis::Core
{
    class EventLoop;
} // namespace AsynGyanis::Core

namespace AsynGyanis::Net
{
    /**
     * @brief 出站 TCP 客户端
     *
     * @details 接受主机名（或 IP 文本）+ 端口，自动解析并尝试连接。
     *          地址族优先顺序为 IPv6 > IPv4（IPv6 位于内部地址列表的尾部，
     *          见 AsyncResolver 的排序约定），每个地址依次尝试，首个成功即返回。
     */
    class TcpClient
    {
    public:
        /**
         * @brief 异步连接一个主机
         * @param loop 发起方的事件循环（连接操作必须在该循环上执行）
         * @param host 主机名或 IP 文本；空主机名返回 nullptr
         * @param port 目标端口（主机字节序）
         * @return Task<std::unique_ptr<TcpStream>> 连接成功后返回流对象；
         *         所有地址都连不上时返回 nullptr
         * @note 本版本没有应用层连接超时，超时由内核 tcp_syn_retries 控制；
         *       需要精确超时的场景将在 HTTP 客户端层补充
         * @note host 按值取 std::string 而不是视图：本函数是惰性 Task，帧体要等 co_await 才跑，
         *       中间隔着「发起到等待」这一步——视图参数在那时早已悬垂
         */
        static Core::Task<std::unique_ptr<TcpStream>> connect(Core::EventLoop &loop, std::string host, uint16_t port);
    };
} // namespace AsynGyanis::Net
