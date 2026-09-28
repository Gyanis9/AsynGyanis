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
#include <expected>
#include <system_error>

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
         * @brief 接管一个**别人已经绑好**的数据报套接字（跨进程共享一条 UDP 端口的那一步）
         *
         * @details 存在的理由：Windows 没有 `SO_REUSEPORT` 的等价物，多个进程各自 bind 同一端口时内核把
         *          全部报文交给最后绑上的那一个，其余进程一个错都不报却永远收不到报文。于是「一条端口、
         *          多个进程」只能由一方 bind、把套接字交给别的进程（`Socket::writeListeningSocketHandoff`
         *          的机制本身不限套接字类型），数据报这一侧的接手动作就是本函数。
         * @param descriptor 已经 bind 过的数据报描述符；**所有权随之转移**，本对象析构或 close() 会关掉它。
         *        失败时不接管也不关闭——那枚描述符还是调用方的
         * @return std::expected<DatagramSocket, std::error_code> 接管好的套接字；失败给出这四类原因之一：
         *         `bad_file_descriptor` 描述符无效、`not_a_socket` 句柄有效但根本不是套接字（普通文件、
         *         目录、管道）、`not_supported` 是套接字但类型不是 SOCK_DGRAM、
         *         `invalid_argument` 还没 bind（本地端口为 0，「谁往这个端口发报文」这回事不存在）
         * @note 「不是套接字」与「不是数据报」分开报：前者要换的是传进来的东西，后者是交出方送错了类型
         * @note 接手方一律被置为**非阻塞**：不置会把事件循环卡在 recvfrom 上。POSIX 上文件状态位由同一个
         *       开放文件描述共享，而本层的交出方本来就非阻塞（`bindTo` 置过），这里补置不会把对方改坏；
         *       Windows 侧重建出的句柄形态随协议信息，可能带着阻塞位
         * @note 也会被取消「随子进程继承」：本层交出去的套接字都不该随 spawn 漏给下一个进程，
         *       与 bindTo 同一条口径
         */
        [[nodiscard]] static std::expected<DatagramSocket, std::error_code> adopt(int descriptor) noexcept;

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
         * @note 截断交付时来源地址照旧有效：Windows 上这一形状由 WSAEMSGSIZE（即「调用失败」）
         *       报回来，内核却已把来源地址写好，调用方据此回包。整条报文算已消费，后续读不会
         *       拿到被截掉的后半截
         */
        [[nodiscard]] ssize_t receive(void *buffer, std::size_t capacity, SocketAddress &peerAddress) const noexcept;

        /**
         * @brief 发一条报文
         * @param peerAddress 目标地址
         * @param buffer 待发数据；length 为 0 时允许 nullptr（那正是空报文的自然写法）
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
