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
     * @note initialize()/finalize() 以引用计数配对：多个持有网络资源的对象可各自成对调用，
     *       Winsock 只在首个 initialize() 时启动、在最后一个 finalize() 时清理。
     */
    class Socket
    {
    public:
        /**
         * @brief Winsock 初始化引用的 RAII 守卫
         *
         * @details 构造时申请一次 initialize() 引用，析构时自动释放。适用于存在多条
         *          返回路径、手工配对 finalize() 容易遗漏的调用方（如域名解析）。
         */
        class Initialization
        {
        public:
            /**
             * @brief 申请一次网络子系统初始化引用
             */
            Initialization() noexcept :
                m_valid(initialize())
            {
            }

            ~Initialization() noexcept
            {
                // 仅在确实取得引用时释放，避免把引用计数减成负数
                if (m_valid)
                {
                    finalize();
                }
            }

            Initialization(const Initialization &) = delete;

            Initialization &operator=(const Initialization &) = delete;

            /**
             * @brief 查询初始化引用是否申请成功
             * @return true 引用已建立，可继续调用 socket API
             */
            [[nodiscard]] bool isValid() const noexcept
            {
                return m_valid;
            }

        private:
            bool m_valid = false; ///< 是否成功取得 Winsock 初始化引用
        };

        /**
         * @brief 申请网络子系统初始化引用（Windows 下按需执行 WSAStartup）
         * @return true 引用已建立（首次调用会真正启动 Winsock）
         * @return false Windows 下 WSAStartup 失败
         */
        static bool initialize() noexcept;

        /**
         * @brief 释放一次网络子系统初始化引用（引用归零时执行 WSACleanup）
         * @details 与 initialize() 成对调用；仍有其他引用存活时不会真正清理，
         *          以免提前拆掉存活 socket 依赖的 Winsock 状态。
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
