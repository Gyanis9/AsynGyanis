/**
 * @file Connection.h
 * @brief TCP连接基类，支持协作取消
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "Core/Socket/AsyncSocket.h"
#include "Core/Coroutine/Cancelable.h"
#include "Core/Coroutine/Task.h"

#include <atomic>
#include <chrono>
#include <optional>
#include <string>

namespace AsynGyanis::Core
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
         * @param socket 已建立的异步socket
         */
        explicit Connection(AsyncSocket socket);

        /**
         * @brief 虚析构函数，默认实现。
         */
        virtual ~Connection() = default;

        Connection(const Connection &) = delete;

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
         * @note 派生类若在套接字之外还持有自有传输层（例如 TLS），必须重写本函数先收掉
         *       自己那一层再调用基类实现；否则经基类指针（ConnectionManager::shutdown()）
         *       关闭时，自有传输层收不到任何通知。
         */
        virtual void close();

        /**
         * @brief 检查连接是否存活。
         * @return true 表示连接有效，false 表示已关闭或无效
         * @note 派生类可重写以叠加自有传输层的判据（例如 TLS 通道是否仍然打开）。
         */
        [[nodiscard]] virtual bool isAlive() const noexcept;

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

        /**
         * @brief 刷新空闲截止时间，把「多久没动静算超期」重新计时。
         *
         * @details 会话在相位切换时调用它：等待新请求首字节前刷空闲容忍度，读到字节后刷读超时，
         *          发送响应前刷写超时。到点之后由服务器上的清扫协程负责关闭连接，
         *          连接自身不做任何定时等待（帧在协程被挂起期间被销毁会让定时等待指向已释放内存）。
         *
         * @param timeout 容忍时长；非正数表示清除截止时间，即关闭本项超时保护
         * @note 只有所属事件循环线程读写本状态（清扫协程也在该线程上），因此没有原子量
         */
        void refreshIdleDeadline(std::chrono::milliseconds timeout) noexcept;

        /**
         * @brief 清除空闲截止时间：此后 isIdleExpired() 一律返回 false
         * @note 线程约束同 refreshIdleDeadline()：只在所属事件循环线程上调用
         */
        void clearIdleDeadline() noexcept;

        /**
         * @brief 判断连接是否已超过空闲截止时间
         * @param now 判定用的当前时刻，由调用方取一次时钟后对同批连接复用，避免逐条取时产生偏差
         * @return true 已设置截止时间且 now 不早于它
         * @return false 没有截止时间（不受超时约束），或尚未到点
         */
        [[nodiscard]] bool isIdleExpired(std::chrono::steady_clock::time_point now) const noexcept;

        /**
         * @brief 标记本连接当前是否有在途工作
         * @details 由协议层维护：HTTP 会话在「已收到完整请求、开始处理」到「响应发完」之间置 true，
         *          让 TcpServer::drain() 能区分「正在服务请求」与「空闲等下一个请求」的连接，
         *          只等待前者做完。默认 false，即视为没有在途工作。
         * @param busy true 表示有在途请求正在处理
         * @note **非原子量**：只有所属事件循环线程读写（drain 协程同样跑在该线程上）
         */
        void setBusy(bool busy) noexcept;

        /**
         * @brief 查询本连接当前是否有在途工作
         * @return true 有在途请求正在处理，优雅关闭必须等它做完
         * @return false 没有在途工作（空闲 keep-alive、只收到半条请求）
         * @note 线程约束同 setBusy()：只在所属事件循环线程上调用
         */
        [[nodiscard]] bool isBusy() const noexcept;

        /**
         * @brief 本连接被服务器的空闲清扫协程按超时关闭时的上报钩子
         *
         * @details 清扫协程扫出超过空闲截止时间的连接后调用它，默认空实现：不关心超时统计的
         *          连接子类无需理会，需要上报的协议层（如 HTTP 会话）重写它即可。上报点做成虚函数
         *          是为了让基础设施不必反过来认识具体协议——服务器一侧因此没有 dynamic_cast。
         * @note 由清扫协程在所属事件循环线程上、**close() 之后**调用：连接此刻已收口，
         *       实现里不得再发起任何收发，且必须无异常（异常会穿透清扫协程）。
         */
        virtual void onIdleTimeoutClosed() noexcept
        {
        }

    private:
        AsyncSocket       m_socket;      ///< 底层异步socket
        Cancelable        m_cancelable;  ///< 取消支持（stop_token）
        std::atomic<bool> m_alive{true}; ///< 连接存活标志，原子操作保证线程安全
        bool              m_busy{false}; ///< 是否有在途工作，由协议层维护（见 setBusy()）

        /// 空闲截止时间；未设置表示这条连接不参与超时清扫。只由所属事件循环线程访问（见 refreshIdleDeadline()）
        std::optional<std::chrono::steady_clock::time_point> m_idleDeadline;
    };

} // namespace AsynGyanis::Core
