/**
 * @file EventBackendContract.h
 * @brief 事件后端的编译期契约：epoll / IOCP / io_uring 三个实现共同满足的成员集合
 * @author Gyanis
 * @date 2026-10-11
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/Platform.h"

// 概念签名里用到 epoll_event：这里必须自含所用类型，不能指望包含方先引了各自的平台头——
// Windows 侧的 Iocp.h 正是先引本头、后引 wepoll.h，漏了这一段整批 TU 编不过（Linux 侧
// Platform.h 恰好带了 <sys/epoll.h>，所以这个缺口只在 Windows 上现形）
#ifdef _WIN32
#include "wepoll.h"
#else
#include <sys/epoll.h>
#endif

#include <concepts>
#include <cstdint>
#include <span>
#include <system_error>

namespace AsynGyanis::Platform
{
    /**
     * @brief 事件后端的概念契约（三个实现都必须满足）
     *
     * @details 三个后端靠「成员集合一致」让 EventLoop 与 IoWatcher 不需要任何平台分支，而这份
     *          一致性只靠注释、共用用例与三平台 CI 兜着时，漏一个成员要到某个平台编不过才
     *          现形，而日常开发往往只碰一个平台。这条 concept 把它抬成编译期判据：每个后端在
     *          自己的头文件里类定义之后 static_assert 一次，各自平台/构建开关的编译各自钉住。
     *
     * @note 只钉「可调用性与返回类型」这一层，也就是上层真正依赖的那一层；不要求 const 修饰
     *       一致——三个后端对自身记账的写法本就不同（见各头文件对应方法的说明），修饰统一由
     *       代码评审管，概念管不着。
     * @note `kMaximumEventCount` 刻意不在契约里：它是各后端的私有成员，上层不读它；
     *       三处同为 1024 的一致性由各自的 wait() 注释与共用用例管。
     */
    template<typename Backend>
    concept EventBackend = requires(Backend backend, int fileDescriptor, std::uint32_t events, void *userData, int timeoutMs, const sockaddr *address) {
        { backend.addFileDescriptor(fileDescriptor, events, userData) } -> std::same_as<bool>;
        { backend.modFileDescriptor(fileDescriptor, events, userData) } -> std::same_as<bool>;
        { backend.delFileDescriptor(fileDescriptor) } -> std::same_as<bool>;
        { backend.wait(timeoutMs) } -> std::same_as<std::span<epoll_event>>;
        { backend.fileDescriptor() } -> std::same_as<Platform::EpollHandle>;
        { backend.supportsCompletionAccept() } -> std::same_as<bool>;
        { backend.supportsCompletionConnect() } -> std::same_as<bool>;
        { backend.lastError() } -> std::same_as<std::error_code>;
        { backend.takeAcceptedSocket(fileDescriptor, &fileDescriptor) } -> std::same_as<bool>;
        { backend.beginConnect(fileDescriptor, address, 0, &timeoutMs) } -> std::same_as<bool>;
        { backend.takeConnectResult(fileDescriptor, &timeoutMs) } -> std::same_as<bool>;
    };

} // namespace AsynGyanis::Platform
