/**
 * @file DatagramSocket.h
 * @brief 数据报套接字：一个端口面对任意多个对端，收发都要带上对端地址
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/IO/Socket.h"

#include <cstddef>

namespace AsynGyanis::Platform
{
    /**
     * @brief 已绑定的 UDP 套接字（RAII）
     *
     * @details 与 Socket 那套「按描述符调静态方法」不同，这里是持有者语义：构造即建套接字并绑定，
     *          析构即关闭。QUIC 这类协议依赖的正是「一个端口上多条连接」——收到的每一条报文都要
     *          连同来源地址一起交给上层（由上层按连接标识分派），因此收发接口都把地址带上。
     *
     * @note 非阻塞：没有数据时 receive() 返回 -1 并置 kWouldBlock，调用方应等可读再收（见
     *       Core 侧的数据报等待封装）。
     * @note 本类不做 WSAStartup：与其它套接字一样，调用方须先完成网络库初始化
     *       （Core::IoContext 在构造时负责，析构时回收）。
     */
    class DatagramSocket
    {
    public:
        /// 单条报文可携带的最大字节数：UDP 上限 65535 减 IPv4 头(20)与 UDP 头(8)。
        /// 实际协议（如 QUIC）会把报文限制得小得多，这里的上限只用于缓冲区尺寸与参数校验
        static constexpr std::size_t kMaximumDatagramBytes = 65535 - 20 - 8;

        DatagramSocket() = default;

        ~DatagramSocket();

        DatagramSocket(DatagramSocket &&other) noexcept;

        DatagramSocket &operator=(DatagramSocket &&other) noexcept;

        DatagramSocket(const DatagramSocket &) = delete;

        DatagramSocket &operator=(const DatagramSocket &) = delete;

        /**
         * @brief 建一个 UDP 套接字并绑定到给定本地地址
         * @details 地址族取自 localAddress：给出 IPv4 地址就建 IPv4 套接字，IPv6 同理。
         *          同时打开地址复用（重启后能立刻重新绑定同一个端口）；地址族与端口由调用方给，
         *          不给默认值——「随便绑哪儿」在服务端是危险默认。
         * @param localAddress 本地地址；端口给 0 表示由内核分配（测试与临时端口用）
         * @return DatagramSocket 已绑定的套接字；失败时 isValid() 为 false，原因见
         *         PlatformError::lastErrorCode()
         */
        [[nodiscard]] static DatagramSocket bindTo(const SocketAddress &localAddress) noexcept;

        /**
         * @brief 套接字是否可用（建成功、绑定成功、未被移动走或关闭）
         * @return true 可用于收发
         */
        [[nodiscard]] bool isValid() const noexcept;

        /**
         * @brief 取底层描述符
         * @return int 描述符；无效时为负数。用于交给事件循环做就绪等待
         */
        [[nodiscard]] int fileDescriptor() const noexcept;

        /**
         * @brief 取本端实际绑定的地址
         * @details 端口给 0 时内核会分配一个，取回来的才是真正在用的
         * @return SocketAddress 本端地址；无效套接字返回未设置的地址（length 为 0）
         */
        [[nodiscard]] SocketAddress localAddress() const noexcept;

        /**
         * @brief 收一条报文（不改动本对象，可在 const 套接字上调用）
         * @param buffer 接收缓冲
         * @param capacity 缓冲容量
         * @param peerAddress 输出参数：来源地址；传入时先被清零，失败时保持未设置
         * @return ssize_t 收到的字节数；无数据返回 -1 并置 kWouldBlock；缓冲小于报文时多出的字节
         *         被丢弃（UDP 语义），返回值即 capacity
         */
        [[nodiscard]] ssize_t receive(void *buffer, std::size_t capacity, SocketAddress &peerAddress) const noexcept;

        /**
         * @brief 发一条报文
         * @param peerAddress 目标地址
         * @param buffer 待发数据
         * @param length 数据长度；0 表示空报文（合法，接收侧照收），
         *        超过 kMaximumDatagramBytes 时当场判错（不交给系统调用去报 EMSGSIZE，
         *        那样在两端会得到不同的错误码，不如这一层统一说清）
         * @return ssize_t 实际发出的字节数；失败返回 -1 并置错误码
         */
        [[nodiscard]] ssize_t send(const SocketAddress &peerAddress, const void *buffer, std::size_t length) const noexcept;

        /**
         * @brief 关闭套接字（幂等）
         */
        void close() noexcept;

    private:
        int m_fileDescriptor{-1}; ///< 描述符；负数表示无效
    };
} // namespace AsynGyanis::Platform
