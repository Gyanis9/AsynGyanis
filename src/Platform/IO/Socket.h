/**
 * @file Socket.h
 * @brief Winsock 生命周期管理与 socket 级跨平台原语
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/Platform.h"

namespace AsynGyanis::Platform
{
    /**
     * @brief socket 层跨平台工具
     *
     * @details Windows 上任何 socket API 调用前必须完成 WSAStartup，Linux 上
     *          相应接口为空操作，因此调用方无需平台分支。
     * @note initialize() 可重复调用，内部以幂等方式处理；进程退出前应调用 finalize()。
     */
    class Socket
    {
    public:
        /**
         * @brief 初始化网络子系统（Windows 下执行 WSAStartup）
         * @return true 初始化成功或已完成
         * @return false Windows 下 WSAStartup 失败
         */
        static bool initialize() noexcept;

        /**
         * @brief 释放网络子系统资源（Windows 下执行 WSACleanup）
         * @details 与 initialize() 成对调用；调用后需重新 initialize() 才能继续使用 socket。
         */
        static void finalize() noexcept;

        /**
         * @brief 接受一条传入连接
         * @details Linux 使用 accept4 一次性置入非阻塞与 close-on-exec 标志；
         *          Windows 无 accept4，接受成功后单独设置非阻塞。
         * @param listenDescriptor 监听描述符
         * @param address 输出参数，对端地址，可为 nullptr
         * @param addressLength 输入输出参数，address 缓冲区容量与实际写入长度
         * @return int 新连接描述符，失败返回 FileDescriptor::kInvalid
         */
        static int accept(int listenDescriptor, sockaddr *address, socklen_t *addressLength) noexcept;
    };
} // namespace AsynGyanis::Platform
