/**
 * @file TcpStream.h
 * @brief 基于 AsyncSocket 的带缓冲 TCP 字节流：按行读取、精确读取与整块写入
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Core/Socket/AsyncSocket.h"

#include <cstddef>
#include <string>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief readUntil 默认的单次最大读取字节数，单位是字节
     *
     * @details 64KB 足够容纳正常的协议头部（HTTP 请求单行通常远小于 1KB），
     *          同时给畸形或恶意流量一个明确的内存上界，避免对端不停发数据时无界增长。
     *          调用方可以传更大的值放开限制，传 0 表示不限长度。
     */
    inline constexpr std::size_t kDefaultMaximumReadSize = 65536;

    /**
     * @brief 带读取缓冲的 TCP 流，在 AsyncSocket 之上提供按行与按长度读取。
     *
     * @details 内部维护一块读取缓冲区，把「读一个字节/一行」这类细粒度需求摊销成
     *          少量系统调用。所有 I/O 都是协程：数据不足时挂起等待可读事件，不阻塞线程。
     *          提供 read / readExact / readUntil / write / writeAll 五类操作。
     * @note 缓冲区里的数据一旦读出就不再属于流：混用「直接 socket 读取」与本类会丢数据。
     * @note 本类接管传入 AsyncSocket 的所有权，析构即关闭连接。
     */
    class TcpStream
    {
    public:
        /**
         * @brief 构造 TcpStream 并接管给定的套接字
         * @param socket 已连接的异步套接字，所有权转移到本对象
         */
        explicit TcpStream(Core::AsyncSocket socket);

        TcpStream(const TcpStream &) = delete;
        TcpStream &operator=(const TcpStream &) = delete;

        /**
         * @brief 移动构造，转移套接字与缓冲区所有权
         * @details 源对象保留有效但内容为空的成员（AsyncSocket 的移动语义把描述符置为无效），
         *          因此移动后源对象不可再用于 I/O。
         * @param other 被移动的流对象
         */
        TcpStream(TcpStream &&other) noexcept = default;

        /**
         * @brief 移动赋值，转移套接字与缓冲区所有权
         * @details 同移动构造；赋值前本对象持有的连接会被自身析构路径关闭。
         * @param other 被移动的流对象
         * @return TcpStream& 引用本对象
         */
        TcpStream &operator=(TcpStream &&other) noexcept = default;

        /**
         * @brief 默认析构，随 m_socket 一起关闭连接
         */
        ~TcpStream() = default;

        /**
         * @brief 从流中读取数据
         * @details 优先消费内部缓冲区，缓冲区耗尽时才向套接字发起一次读取；
         *          因此单次返回的字节数可能小于请求值。
         * @param buffer 接收缓冲区首地址，调用方保证其容量
         * @param length 缓冲区可容纳的字节数
         * @return Core::Task<ssize_t> 实际写入 buffer 的字节数；0 表示对端正常关闭；负数表示读错误
         */
        Core::Task<ssize_t> read(void *buffer, std::size_t length);

        /**
         * @brief 精确读取指定长度字节
         * @details 反复调用 read() 直到凑满 length，用于读定长帧头。
         * @param buffer 接收缓冲区首地址，调用方保证其容量
         * @param length 需要读满的字节数
         * @return Core::Task<> 协程，读满后完成
         * @throws Base::Exception 对端提前关闭或读错误导致无法凑满
         */
        Core::Task<> readExact(void *buffer, std::size_t length);

        /**
         * @brief 读取直到出现指定分隔符
         * @details 结果不含分隔符本身（分隔符被就地消费）。常用于读取一行文本协议。
         * @param delimiter 分隔符字符，例如 '\n'
         * @param maximumSize 允许读取的最大字节数，单位是字节；达到上限时返回已读到的部分
         *                    且丢弃超限内容，传 0 表示不限长度。默认 kDefaultMaximumReadSize
         * @return Core::Task<std::string> 分隔符之前的内容；对端在遇到分隔符前关闭时返回已读到的部分
         * @throws Base::SystemException 底层接收出错（非 EOF）
         */
        Core::Task<std::string> readUntil(char delimiter, std::size_t maximumSize = kDefaultMaximumReadSize);

        /**
         * @brief 写入数据，可能只写入部分
         * @param buffer 待写入数据首地址
         * @param length 待写入字节数
         * @return Core::Task<ssize_t> 实际写入的字节数；负数表示错误
         */
        [[nodiscard]] Core::Task<ssize_t> write(const void *buffer, std::size_t length) const;

        /**
         * @brief 写入全部数据，反复重试直到写完或出错
         * @param buffer 待写入数据首地址
         * @param length 待写入字节数
         * @return Core::Task<> 协程，全部写入后完成
         * @throws Base::Exception 发送失败或对端已关闭连接
         */
        Core::Task<> writeAll(const void *buffer, std::size_t length) const;

        /**
         * @brief 关闭底层连接并丢弃缓冲内容
         */
        void close();

        /**
         * @brief 获取底层套接字引用
         * @return Core::AsyncSocket& 供需要绕过缓冲直接操作的调用方使用
         * @warning 绕过本类直接读写会破坏缓冲区与流的字节序一致性
         */
        [[nodiscard]] Core::AsyncSocket &socket() noexcept;

    private:
        /**
         * @brief 用一次底层接收填满内部缓冲区
         * @details 成功时把缓冲区收缩到实际收到的长度；收到 EOF 时清空缓冲区；
         *          读错误时抛出异常。调用方据缓冲区是否为空判定 EOF。
         * @return Core::Task<> 协程，读取尝试结束后完成
         * @throws Base::SystemException 底层接收返回错误
         */
        Core::Task<> fillBuffer();

        Core::AsyncSocket m_socket;      ///< 底层异步套接字，持有描述符所有权
        std::vector<char> m_readBuffer;  ///< 读取缓冲区，容量为一次系统调用的上限，长度为本次实际收到的字节数
        std::size_t       m_readPosition; ///< m_readBuffer 中已被上层消费的位置，单位字节
    };
} // namespace AsynGyanis::Net
