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
     * @class Epoll
     * @brief epoll 实例 RAII 封装
     *
     * 封装 Linux epoll 系统调用, 提供类型安全的 fd 管理。
     * 内部使用 epoll_create1(EPOLL_CLOEXEC) 创建实例,
     * 通过 epoll_wait 等待事件, 通过 epoll_ctl 管理监听 fd。
     *
     * @note 不可复制, 支持移动
     * @note 当前仅支持 level-triggered 模式, EPOLLET 由 EpollAwaiter 在注册时添加
     */
    class Epoll
    {
    public:
        /**
         * @brief 创建 epoll 实例
         * @throws std::runtime_error 当 epoll_create1 失败时抛出
         */
        Epoll();

        /**
         * @brief 析构并关闭 epoll 实例
         */
        ~Epoll();

        /** @brief 移动构造 */
        Epoll(Epoll &&) noexcept;
        /** @brief 移动赋值 */
        Epoll &operator=(Epoll &&) noexcept;

        Epoll(const Epoll &)            = delete;
        Epoll &operator=(const Epoll &) = delete;

        /**
         * @brief 向 epoll 添加要监听的文件描述符
         * @param fd 目标文件描述符
         * @param events 监听的事件掩码 (EPOLLIN, EPOLLOUT 等)
         * @param userData 挂载到 epoll_event.data.ptr 的用户数据 (通常为协程句柄地址)
         * @return 成功返回 true, 失败返回 false
         */
        bool addFd(int fd, uint32_t events, void *userData = nullptr) const;

        /**
         * @brief 修改已监听 fd 的事件掩码
         * @param fd 目标文件描述符
         * @param events 新的事件掩码
         * @param userData 新的用户数据
         * @return 成功返回 true, 失败返回 false
         */
        bool modFd(int fd, uint32_t events, void *userData = nullptr) const;

        /**
         * @brief 从 epoll 移除 fd
         * @param fd 目标文件描述符
         * @return 成功返回 true, 失败返回 false
         */
        bool delFd(int fd) const;

        /**
         * @brief 阻塞等待 IO 事件
         * @param timeoutMs 超时毫秒数, -1 表示无限等待, 0 表示立即返回
         * @return 就绪事件列表的视图, 长度为 0 表示超时或无事件
         */
        [[nodiscard]] std::span<epoll_event> wait(int timeoutMs = -1);

        /**
         * @brief 获取 epoll 文件描述符
         * @return epoll fd, 可用于集成到其他事件循环
         */
        [[nodiscard]] int fd() const noexcept;

    private:
        /** @brief 关闭 epoll fd */
        void destroy();

        int                      m_fd{-1};
        std::vector<epoll_event> m_events;
        static constexpr int     DEFAULT_MAX_EVENTS = 1024;
    };

}

#endif
