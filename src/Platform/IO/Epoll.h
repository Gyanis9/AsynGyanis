/**
 * @file Epoll.h
 * @brief Linux epoll 实例的 RAII 封装
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "AsynGyanisExport.h"

#include "Platform/IO/EventBackendContract.h"
#include "Platform/Platform.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#if ASYN_PLATFORM_WIN32

#include "Platform/IO/Iocp.h"

namespace AsynGyanis::Platform
{
    /**
     * @brief Windows 上的 Epoll 就是完成端口后端
     *
     * @details 成员集合与 Linux 实现一致（add/mod/del/wait/fileDescriptor），
     *          因此 EventLoop / IoWatcher / EpollAwaiter 不需要任何平台分支；
     *          另有 Windows 专属的 Iocp::takeAcceptedSocket()——AcceptEx 接入的连接
     *          只能由它取走，见 TcpAcceptor 的接受路径。
     */
    using Epoll = Iocp;

    // 契约复检：走这一步的平台也把概念钉一次，本头文件在每个平台分支里都自己站得住
    static_assert(EventBackend<Epoll>);
} // namespace AsynGyanis::Platform

#elif defined(ASYN_WITH_IO_URING)

#include "Platform/IO/Uring.h"

namespace AsynGyanis::Platform
{
    /**
     * @brief 开启 ASYN_WITH_IO_URING 时的 Linux 后端：io_uring
     *
     * @details 成员集合与 epoll 实现一致（add/mod/del/wait/fileDescriptor），
     *          因此 EventLoop / IoWatcher 不需要任何分支；由构建开关选用（默认走 epoll）。
     */
    using Epoll = Uring;

    // 契约复检：走这一步的平台也把概念钉一次，本头文件在每个平台分支里都自己站得住
    static_assert(EventBackend<Epoll>);
} // namespace AsynGyanis::Platform

#else

namespace AsynGyanis::Platform
{
    /**
     * @brief epoll 实例 RAII 封装
     */
    class ASYN_PLATFORM_API Epoll
    {
    public:
        /**
         * @brief 创建 epoll 实例
         * @throws std::system_error epoll_create1 失败（文件描述符耗尽，或内核不允许创建 epoll
         *         实例）。没有可用实例的循环无法等待任何事件，因此当场抛而不是留下一个空壳对象
         */
        Epoll();

        /**
         * @brief 析构 epoll 实例，自动关闭文件描述符
         */
        ~Epoll();

        Epoll(Epoll &&) noexcept;

        Epoll &operator=(Epoll &&) noexcept;

        Epoll(const Epoll &) = delete;

        Epoll &operator=(const Epoll &) = delete;

        /**
         * @brief 向 epoll 添加要监听的文件描述符
         * @param fileDescriptor 目标文件描述符
         * @param events         监听的事件掩码（EPOLLIN、EPOLLOUT 等）
         * @param userData       挂载到 epoll_event.data.ptr 的用户数据，通常为协程句柄地址
         * @return 成功返回 true，失败返回 false（errno 会被保留）
         */
        bool addFileDescriptor(int fileDescriptor, uint32_t events, void *userData = nullptr);

        /**
         * @brief 修改已监听文件描述符的事件掩码
         * @param fileDescriptor 目标文件描述符
         * @param events         新的事件掩码
         * @param userData       新的用户数据（若需保持不变，可传入原值）
         * @return 成功返回 true，失败返回 false
         */
        bool modFileDescriptor(int fileDescriptor, uint32_t events, void *userData = nullptr);

        /**
         * @brief 从 epoll 移除文件描述符
         * @param fileDescriptor 目标文件描述符
         * @return 成功返回 true，失败返回 false
         */
        bool delFileDescriptor(int fileDescriptor);

        /**
         * @brief 阻塞等待 IO 事件
         * @param timeoutMs 超时毫秒数，-1 表示无限等待，0 表示立即返回（非阻塞）
         * @return 就绪事件列表的视图（std::span<epoll_event>），
         *         长度为 0 表示超时或无事件，长度 >0 表示有事件发生
         * @throws std::system_error epoll_wait 失败（句柄已销毁或内核报错）。EINTR 不算失败：
         *         它返回空视图，让调用方回到循环开头重新检查停止请求
         * @warning 视图指向本对象的固定容量缓冲（每次取满 kMaximumEventCount 条为止）。
         *          下一次 wait() 会覆盖它的内容，因此在持有视图期间不得再进入 wait()，
         *          也不得把这个视图跨线程留着用——EventLoop::run() 正是靠「先取完这批、
         *          再回到循环末尾」满足这条约束的
         * @note 一次最多返回 kMaximumEventCount 条。取满不算丢事件：epoll 水平触发，
         *       没取走的描述符仍在就绪名单里，下一次 wait() 会立刻再报
         */
        [[nodiscard]] std::span<epoll_event> wait(int timeoutMs = 0);

        /**
         * @brief 获取 epoll 句柄
         * @return epoll 文件描述符
         */
        [[nodiscard]] Platform::EpollHandle fileDescriptor() const noexcept;

        /**
         * @brief 安装诊断回调（epoll 没有异步探针，保留空实现以统一接口）
         * @param handler 诊断文本的接收者
         */
        void setDiagnosticHandler(std::function<void(std::string_view)> handler) noexcept
        {
            static_cast<void>(handler);
        }

        /**
         * @brief 清空诊断回调（epoll 的空实现）
         */
        void clearDiagnosticHandler() noexcept
        {
        }

    private:
        /**
         * @brief 关闭 epoll 文件描述符并重置状态
         */
        void destroy();

        Platform::EpollHandle    m_fileDescriptor{Platform::kInvalidEpollHandle}; ///< epoll 实例句柄
        std::vector<epoll_event> m_events;                  ///< wait() 的落地缓冲：构造时一次定容为 kMaximumEventCount，此后不再改容量（改容量会让已交出去的视图悬垂）
        static constexpr int     kMaximumEventCount = 1024; ///< 单次 wait 最多返回的事件数，同时也是缓冲的固定容量
    };

    // 契约复检：Linux epoll 分支自己的编译期判据（另两个分支在各自的后端头里钉同一个概念）
    static_assert(EventBackend<Epoll>);
} // namespace AsynGyanis::Platform

#endif
