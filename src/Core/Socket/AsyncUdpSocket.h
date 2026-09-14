/**
 * @file AsyncUdpSocket.h
 * @brief 事件循环上的数据报套接字：整条收、整条发，收发都带对端地址
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/IoWatcher.h"
#include "Platform/IO/DatagramSocket.h"

#include <cstddef>
#include <memory>

namespace AsynGyanis::Core
{
    /**
     * @brief 事件循环上的数据报套接字
     *
     * @details 与 AsyncSocket（流式）同一套结构：内部用常驻 IoWatcher 吸收「暂时不可读写」，
     *          注册推迟到第一次等待时才发生（见 ensureWatcher()）。语义上的差别来自数据报本身：
     *          @li 发送是**整条**要么出去要么不出去（内核不会把一条报文切开），因此发不下的处理是
     *              「等可写后整条重发」，而不是续发剩余字节；
     *          @li 接收要连同来源地址一起交回，调用方据此把报文分派给对应的连接。
     *
     * @warning 与其它循环对象同一条线程契约：只在**所属事件循环线程**上创建、使用与关闭
     *          （内部状态没有原子保护）。
     * @note 一条报文的最大长度见 Platform::DatagramSocket::kMaximumDatagramBytes。
     */
    class AsyncUdpSocket
    {
    public:
        /**
         * @brief 接手一个已绑定的平台套接字
         * @param loop 所属事件循环
         * @param socket 已绑定且已置非阻塞的套接字，所有权随之转移
         */
        AsyncUdpSocket(EventLoop &loop, Platform::DatagramSocket socket);

        ~AsyncUdpSocket() = default;

        AsyncUdpSocket(AsyncUdpSocket &&other) noexcept;

        AsyncUdpSocket &operator=(AsyncUdpSocket &&other) noexcept;

        AsyncUdpSocket(const AsyncUdpSocket &) = delete;

        AsyncUdpSocket &operator=(const AsyncUdpSocket &) = delete;

        /**
         * @brief 套接字是否可用
         * @return true 描述符有效（未被移动走、未关闭）
         */
        [[nodiscard]] bool isValid() const noexcept;

        /**
         * @brief 取底层描述符
         * @return int 描述符；无效时为负数
         */
        [[nodiscard]] int fileDescriptor() const noexcept;

        /**
         * @brief 取本端绑定地址
         * @return Platform::SocketAddress 本端地址；无效套接字返回未设置的地址
         */
        [[nodiscard]] Platform::SocketAddress localAddress() const noexcept;

        /**
         * @brief 收一条报文，内部吸收「暂时没有数据」
         * @details 缓冲放不下整条报文时多出的字节被丢弃（UDP 语义），返回值即 capacity。
         * @param buffer 接收缓冲
         * @param capacity 缓冲容量
         * @param peerAddress 输出参数：来源地址
         * @return Task<ssize_t> 收到的字节数（**0 是合法的空报文**）；等待可读期间套接字被关闭时返回 -1
         * @throws Base::Exception 平台层报错（描述符非法等），原因已含中文说明
         */
        [[nodiscard]] Task<ssize_t> asyncReceiveFrom(void *buffer, std::size_t capacity, Platform::SocketAddress &peerAddress);

        /**
         * @brief 发一条报文，内部吸收「发送缓冲暂时放不下」
         * @details 数据报不会部分写出，因此等待可写后是**整条重发**；返回值与 length 相等即成功。
         * @param peerAddress 目标地址
         * @param buffer 待发数据
         * @param length 数据长度
         * @return Task<ssize_t> 实际发出的字节数；等待可写期间套接字被关闭时返回 -1
         * @throws Base::Exception 平台层报错（描述符非法、长度超限等），原因已含中文说明
         */
        [[nodiscard]] Task<ssize_t> asyncSendTo(const Platform::SocketAddress &peerAddress, const void *buffer, std::size_t length);

    private:
        /**
         * @brief 首次等待时才建立 IoWatcher
         * @details 与 AsyncSocket 同一理由：注册要写进 epoll，而很多套接字一辈子不会被等待
         * @return IoWatcher* 注册对象；套接字无效时为空
         */
        [[nodiscard]] IoWatcher *ensureWatcher() const;

        /// 等可读；返回 false 表示描述符已关闭（调用方应停止重试）
        [[nodiscard]] IoWatcher::Awaiter waitReadable() const;

        /// 等可写；返回 false 表示描述符已关闭（调用方应停止重试）
        [[nodiscard]] IoWatcher::Awaiter waitWritable() const;

        EventLoop                *m_loop{nullptr}; ///< 所属事件循环（非拥有）
        Platform::DatagramSocket  m_socket;        ///< 平台套接字（拥有描述符）
        mutable std::unique_ptr<IoWatcher> m_watcher; ///< 常数注册对象；首次等待时建立
    };
} // namespace AsynGyanis::Core
