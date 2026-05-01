/**
 * @file TcpStream.h
 * @brief 基于 AsyncSocket 的带缓冲 TCP 流，提供更高级的读写接口
 * @copyright Copyright (c) 2026
 */
#ifndef NET_TCPSTREAM_H
#define NET_TCPSTREAM_H

#include "Core/AsyncSocket.h"
#include "Core/Task.h"

#include <cstddef>
#include <string>
#include <vector>

namespace Net
{
    /**
     * @brief 带缓冲的 TCP 流，封装 AsyncSocket，提供按行读取、精确读取等高级功能。
     *
     * 该类内部维护一个读取缓冲区，减少系统调用次数，支持：
     * - read()：读取最多 len 字节
     * - readExact()：读取精确长度的数据
     * - readUntil()：读取直到遇到指定分隔符（如 '\n'）
     * - write() / writeAll()：写入数据
     *
     * 所有 I/O 操作均为异步协程任务，可通过 co_await 等待完成。
     */
    class TcpStream
    {
    public:
        /**
         * @brief 构造 TcpStream，接管给定的 AsyncSocket。
         * @param socket 已连接的异步套接字
         */
        explicit TcpStream(Core::AsyncSocket socket);

        TcpStream(const TcpStream &)            = delete;
        TcpStream &operator=(const TcpStream &) = delete;
        TcpStream(TcpStream &&)                 = default;
        TcpStream &operator=(TcpStream &&)      = default;

        /**
         * @brief 默认析构函数。
         */
        ~TcpStream() = default;

        /**
         * @brief 从流中读取数据。
         * @param buf 接收缓冲区指针
         * @param len 最多读取的字节数
         * @return Task<ssize_t> 实际读取的字节数（0 表示连接关闭，负数表示错误）
         *
         * 优先从内部缓冲区读取，若缓冲区不足则从 socket 读取更多数据。
         */
        Core::Task<ssize_t> read(void *buf, size_t len);

        /**
         * @brief 精确读取指定长度的数据。
         * @param buf 接收缓冲区指针
         * @param len 需要读取的字节数
         * @return Task<> 协程，成功时完成，若连接关闭或出错则抛出异常
         *
         * 反复读取直到接收满 len 字节，或遇到错误/EOF。
         */
        Core::Task<> readExact(void *buf, size_t len);

        /**
         * @brief 读取直到遇到指定分隔符。
         * @param delimiter 分隔符字符（如 '\n'）
         * @param maxSize 最大读取字节数（0 表示无限制），超出限制则返回已读取部分
         * @return Task<std::string> 包含分隔符在内的字符串（若读到分隔符），
         *         若连接关闭且未遇到分隔符，则返回已读取的部分；若出错则抛出异常
         *
         * 常用于读取一行文本。
         */
        Core::Task<std::string> readUntil(char delimiter, size_t maxSize = 65536);

        /**
         * @brief 写入数据（可能只写入部分）。
         * @param buf 数据缓冲区指针
         * @param len 要写入的字节数
         * @return Task<ssize_t> 实际写入的字节数（负数表示错误）
         */
        Core::Task<ssize_t> write(const void *buf, size_t len) const;

        /**
         * @brief 写入所有数据，反复重试直到全部写入或出错。
         * @param buf 数据缓冲区指针
         * @param len 要写入的字节数
         * @return Task<> 协程，成功时完成，出错时抛出异常
         */
        Core::Task<> writeAll(const void *buf, size_t len) const;

        /**
         * @brief 关闭底层 socket。
         */
        void close();

        /**
         * @brief 获取底层的 AsyncSocket 引用。
         * @return AsyncSocket&
         */
        Core::AsyncSocket &socket();

    private:
        /**
         * @brief 从底层 socket 读取数据填充内部缓冲区。
         * @return Task<> 协程，成功时缓冲区有数据或 EOF，失败抛出异常
         *
         * 当内部缓冲区数据不足时调用，尝试读取至 m_readBuffer 末尾。
         */
        Core::Task<> fillBuffer();

        Core::AsyncSocket m_socket;     ///< 底层异步套接字
        std::vector<char> m_readBuffer; ///< 读取缓冲区
        size_t            m_readPos;    ///< 当前缓冲区中已消费位置
    };
}

#endif
