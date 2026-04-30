/**
 * @file Connection.h
 * @brief TCP连接基类，支持协作取消
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_CONNECTION_H
#define CORE_CONNECTION_H

#include "AsyncSocket.h"
#include "Cancelable.h"
#include "Task.h"

#include <atomic>
#include <string>

namespace Core
{
    class EventLoop;

    /**
     * @brief TCP连接基类。
     *
     * 封装一个异步TCP连接，提供启动、关闭、地址查询以及协作取消能力。
     * 派生类应实现具体的协议处理逻辑（通过重写 start() 协程）。
     */
    class Connection
    {
    public:
        /**
         * @brief 构造一个连接对象。
         * @param loop 所属的事件循环
         * @param socket 已建立的异步socket
         */
        Connection(EventLoop &loop, AsyncSocket socket);

        /**
         * @brief 虚析构函数，默认实现。
         */
        virtual ~Connection() = default;

        Connection(const Connection &)            = delete;
        Connection &operator=(const Connection &) = delete;

        /**
         * @brief 移动构造函数。
         * @param other 要移动的连接对象
         */
        Connection(Connection &&other) noexcept;

        /**
         * @brief 移动赋值运算符。
         * @param other 要移动的连接对象
         * @return 当前对象的引用
         */
        Connection &operator=(Connection &&other) noexcept;

        /**
         * @brief 启动连接的主逻辑协程。
         *
         * 派生类应重写此函数，实现具体的读写和处理流程。
         * 默认实现返回一个立即完成的协程。
         *
         * @return Task<> 协程任务
         */
        virtual Task<> start();

        /**
         * @brief 主动关闭连接。
         *
         * 设置存活标志为 false，并调用 socket 的关闭接口。
         */
        void close();

        /**
         * @brief 检查连接是否存活。
         * @return true 表示连接有效，false 表示已关闭或无效
         */
        [[nodiscard]] bool isAlive() const noexcept;

        /**
         * @brief 获取底层异步socket的引用。
         * @return AsyncSocket&
         */
        [[nodiscard]] AsyncSocket &socket() noexcept;

        /**
         * @brief 获取取消支持对象的引用。
         * @return Cancelable&
         */
        [[nodiscard]] Cancelable &cancelable() noexcept;

        /**
         * @brief 获取对端的IP地址和端口字符串。
         * @return 字符串格式 "ip:port"
         */
        [[nodiscard]] std::string remoteAddress() const;

        /**
         * @brief 获取本地的IP地址和端口字符串。
         * @return 字符串格式 "ip:port"
         */
        [[nodiscard]] std::string localAddress() const;

    private:
        AsyncSocket       m_socket;      ///< 底层异步socket
        Cancelable        m_cancelable;  ///< 取消支持（stop_token）
        std::atomic<bool> m_alive{true}; ///< 连接存活标志，原子操作保证线程安全
    };

}

#endif
