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
     * @note 报文是**整条**收发：内核不会把一条报文切开，因此发不下时等可写后整条重发，
     *       接收也要连同来源地址一起交回
     * @warning 与其它循环对象同一条线程契约：只在**所属事件循环线程**上创建、使用与关闭
     *          （内部状态没有原子保护）
     * @note 一条报文的最大长度见 Platform::DatagramSocket::kMaximumDatagramBytes
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
         * @brief 一次数据报接收的结果
         * @note 按值返回而不是写进调用方给的引用：本方法是惰性协程，调用方可能先拿到 Task、
         *       稍后才 await，那时那个实参（临时量或已离开作用域的局部对象）已经亡故
         */
        struct DatagramReceiveResult
        {
            ssize_t                 receivedByteCount{-1}; ///< 收到的字节数；负值表示失败（-1）
            Platform::SocketAddress peerAddress;           ///< 来源地址（失败时无意义）
        };

        /**
         * @brief 接收一个数据报并带回来源地址（内部吸收「暂时没有数据」）
         * @param buffer 目标缓冲
         * @param capacity 缓冲容量
         * @return 字节数与来源地址（见结构体说明：按值返回）
         * @note 等待可读期间套接字被关闭时 receivedByteCount 为 -1；**0 是合法的空报文**
         * @note 缓冲放不下整条报文时多出的字节被丢弃（UDP 语义），返回值即 capacity
         */
        [[nodiscard]] Task<DatagramReceiveResult> asyncReceiveFrom(void *buffer, std::size_t capacity);

        /**
         * @brief 发一条报文，内部吸收「发送缓冲暂时放不下」
         * @param peerAddress 目标地址
         * @param buffer 待发数据
         * @param length 数据长度
         * @return 实际发出的字节数（与 length 相等即成功）；等待可写期间套接字被关闭时返回 -1
         * @note 数据报不会部分写出，因此等待可写后是**整条重发**
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

        /// 等可读；co_await 结果为 false 表示注册已失效（描述符已关闭），调用方应停止重试
        [[nodiscard]] IoWatcher::Awaiter waitReadable() const;

        /// 等可写；co_await 结果为 false 表示注册已失效（描述符已关闭），调用方应停止重试
        [[nodiscard]] IoWatcher::Awaiter waitWritable() const;

        EventLoop                *m_loop{nullptr}; ///< 所属事件循环（非拥有）
        Platform::DatagramSocket  m_socket;        ///< 平台套接字（拥有描述符）
        mutable std::unique_ptr<IoWatcher> m_watcher; ///< 常驻注册对象；首次等待时建立
    };
} // namespace AsynGyanis::Core
