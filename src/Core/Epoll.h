/**
 * @file Epoll.h
 * @brief Linux epoll 实例 RAII 封装
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
     * @brief epoll 实例 RAII 封装。
     *
     * 管理 epoll fd 的生命周期，提供添加/修改/删除被监听 fd 的接口。
     * 析构时自动关闭 epoll 文件描述符。
     */
    class Epoll
    {
    public:
        /**
         * @brief 创建 epoll 实例。
         * @throws SystemException 当 epoll_create1 失败时抛出
         */
        Epoll();

        ~Epoll();

        Epoll(const Epoll &)            = delete;
        Epoll &operator=(const Epoll &) = delete;
        Epoll(Epoll &&) noexcept;
        Epoll &operator=(Epoll &&) noexcept;

        /**
         * @brief 向 epoll 添加要监听的文件描述符。
         * @param fd 目标文件描述符
         * @param events 监听的事件掩码（EPOLLIN | EPOLLOUT | EPOLLET 等）
         * @param userData 挂载到 epoll_event.data.ptr 的用户数据
         * @return bool 成功返回 true
         */
        bool addFd(int fd, uint32_t events, void *userData = nullptr) const;

        /**
         * @brief 修改已监听 fd 的事件掩码。
         * @param fd 目标文件描述符
         * @param events 新的事件掩码
         * @param userData 新的用户数据
         * @return bool 成功返回 true
         */
        bool modFd(int fd, uint32_t events, void *userData = nullptr) const;

        /**
         * @brief 从 epoll 移除 fd。
         * @param fd 目标文件描述符
         * @return bool 成功返回 true
         */
        bool delFd(int fd) const;

        /**
         * @brief 阻塞等待 IO 事件。
         * @param timeoutMs 超时毫秒数，-1 表示无限等待
         * @return std::span<epoll_event> 就绪事件列表的视图
         */
        [[nodiscard]] std::span<epoll_event> wait(int timeoutMs = -1);

        /**
         * @brief 获取 epoll 文件描述符（用于集成到其他事件循环）。
         * @return int epoll fd
         */
        [[nodiscard]] int fd() const noexcept;

    private:
        void destroy();

        int                      m_fd{-1};
        std::vector<epoll_event> m_events;
        static constexpr int     DEFAULT_MAX_EVENTS = 1024;
    };

}

#endif
