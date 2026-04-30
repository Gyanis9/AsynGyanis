/**
 * @file Epoll.h
 * @brief Linux epoll 实例的 RAII 封装
 *
 * 管理 epoll fd 的生命周期, 提供添加/修改/删除被监听 fd 的接口。
 * 析构时自动关闭 epoll 文件描述符。支持移动语义, 禁止拷贝。
 *
 * @copyright Copyright (c) 2026
 */

#ifndef CORE_EPOLL_H
#define CORE_EPOLL_H

#include <sys/epoll.h>
#include <cstdint>
#include <span>
#include <vector>

namespace Core
{
    /**
     * @brief epoll 实例 RAII 封装
     *
     * 封装 Linux epoll 系统调用, 提供类型安全的 fd 管理。
     * 内部使用 epoll_create1(EPOLL_CLOEXEC) 创建实例,
     * 通过 epoll_wait 等待事件, 通过 epoll_ctl 管理监听 fd。
     *
     */
    class Epoll
    {
    public:
        /**
         * @brief 创建 epoll 实例
         */
        Epoll();

        /**
         * @brief 析构 epoll 实例，自动关闭文件描述符
         */
        ~Epoll();

        Epoll(Epoll &&) noexcept;
        Epoll &operator=(Epoll &&) noexcept;
        Epoll(const Epoll &)            = delete;
        Epoll &operator=(const Epoll &) = delete;

        /**
         * @brief 向 epoll 添加要监听的文件描述符
         * @param fd       目标文件描述符
         * @param events   监听的事件掩码（EPOLLIN、EPOLLOUT 等）
         * @param userData 挂载到 epoll_event.data.ptr 的用户数据，通常为协程句柄地址
         * @return 成功返回 true，失败返回 false（errno 会被保留）
         */
        bool addFd(int fd, uint32_t events, void *userData = nullptr) const;

        /**
         * @brief 修改已监听 fd 的事件掩码
         * @param fd       目标文件描述符
         * @param events   新的事件掩码
         * @param userData 新的用户数据（若需保持不变，可传入原值）
         * @return 成功返回 true，失败返回 false
         */
        bool modFd(int fd, uint32_t events, void *userData = nullptr) const;

        /**
         * @brief 从 epoll 移除 fd
         * @param fd 目标文件描述符
         * @return 成功返回 true，失败返回 false
         */
        bool delFd(int fd) const;

        /**
         * @brief 重新装配 fd 并启用 EPOLLONESHOT 模式
         *
         * 如果 fd 已经注册，则执行 MOD 操作；否则执行 ADD 操作。
         * 通常用于一次性触发（one-shot）场景，事件触发后需要重新装配才能再次触发。
         * @param fd       目标文件描述符
         * @param events   新的事件掩码（调用者通常应包含 EPOLLONESHOT）
         * @param userData 挂载的用户数据
         * @return 成功返回 true，失败返回 false
         */
        bool rearmFd(int fd, uint32_t events, void *userData = nullptr) const;

        /**
         * @brief 阻塞等待 IO 事件
         * @param timeoutMs 超时毫秒数，-1 表示无限等待，0 表示立即返回（非阻塞）
         * @return 就绪事件列表的视图（std::span<epoll_event>），
         *         长度为 0 表示超时或无事件，长度 >0 表示有事件发生
         */
        [[nodiscard]] std::span<epoll_event> wait(int timeoutMs = 0);

        /**
         * @brief 获取 epoll 文件描述符
         * @return epoll fd，可用于集成到其他事件循环
         */
        [[nodiscard]] int fd() const noexcept;

    private:
        /**
         * @brief 关闭 epoll fd 并重置状态
         */
        void destroy();

        int                      m_fd{-1};                  ///< epoll 实例的文件描述符
        std::vector<epoll_event> m_events;                  ///< 存储 wait() 返回的事件数组，容量为 DEFAULT_MAX_EVENTS
        static constexpr int     DEFAULT_MAX_EVENTS = 1024; ///< 默认每次 wait 最多返回的事件数
    };
}

#endif
