/**
 * @file UpgradeChannel.h
 * @brief 零停机换代的交接通道：把已在监听的套接字交给新一代进程
 * @author Gyanis
 * @date 2026-09-27
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/IO/Socket.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace AsynGyanis::Core
{
    /**
     * @brief 交棒方这一侧的交接通道（一次性：只交给一个新一代）
     *
     * @details 换代的地基是「监听套接字跨进程移交」：新一代拿到同一个端点的监听引用之后，端口从不
     *          关闭，也就没有 ECONNREFUSED 的空窗。本类只管**通道**——把监听套接字送过去这一步仍由
     *          平台层做（POSIX 走 SCM_RIGHTS，Windows 走 WSADuplicateSocketW，见
     *          Platform::Socket::writeListeningSocketHandoff）。
     * @note 通道不带鉴权：本层不校验「对面就是我要交给的那个进程」。POSIX 上那道门是套接字文件的
     *       权限（开出来就是 0700 那一档），Windows 上通道只绑回环，本机可连的范围就是本机。
     * @warning 入口都会阻塞：waitForPeer() 给出正数预算时最多阻塞到那个上限，其余情况请只在
     *          起事件循环线程之前用（交棒方通常先建通道、派生新一代，再开循环服务）。
     *          把它们放到循环线程上会让那条循环停在那儿等。
     */
    class UpgradeChannel
    {
    public:
        /**
         * @brief 建一条已在监听的交接通道
         * @return std::expected<UpgradeChannel, std::string> 成功交出新通道；失败交中文原因（带错误码）
         */
        [[nodiscard]] static std::expected<UpgradeChannel, std::string> open();

        UpgradeChannel() noexcept = default;

        UpgradeChannel(const UpgradeChannel &)            = delete;
        UpgradeChannel &operator=(const UpgradeChannel &) = delete;

        UpgradeChannel(UpgradeChannel &&other) noexcept;

        UpgradeChannel &operator=(UpgradeChannel &&other) noexcept;

        /// 析构即收口：关掉监听端并删掉 POSIX 上留下的套接字文件
        ~UpgradeChannel() noexcept;

        /**
         * @brief 新一代连这条通道要用的地址文本
         * @return const std::string & 直接交给 Platform::Process::spawn 的命令行参数即可
         */
        [[nodiscard]] const std::string &address() const noexcept;

        /**
         * @brief 等新一代连上，交回那条已连通的通道
         * @details 拿到对端之后监听端就没用了，本函数顺手把它收掉（一条通道只交给一个新一代）：
         *          此后再有进程连这个地址只会失败，而不是插进一次已经谈定的交接。
         * @param budget 等待上限；**非正数表示无限等**（本方法原有的阻塞语义，交棒方还没派生
         *        新一代时就要这种等法）。给出正数时，期限内没人连就收掉通道并报超时——
         *        对端是别的进程，它完全可能起崩后再也不连，无限等会把调用方的编排循环冻住
         * @return std::expected<int, std::string> 通道描述符；失败交中文原因
         * @note 与 Platform::Socket::waitForAcceptReady() 的 0 含义不同，那边是「只取当前状态」：
         *       本层不暴露那种读法，正数预算之外的取值一律按无限等处理
         */
        [[nodiscard]] std::expected<int, std::string> waitForPeer(std::chrono::milliseconds budget = std::chrono::milliseconds::zero()) noexcept;

        /**
         * @brief 把已在监听的套接字交给 waitForPeer() 交回的那条通道
         * @param peerDescriptor 已连通的通道描述符（本函数不关它，交完由调用方收）
         * @param listenDescriptor 要移交的监听套接字，必须已经 listen() 过
         * @param targetProcessId 接收方的进程号（Windows 按进程号认目标；POSIX 上内核自己处理，可填 0）
         * @return std::expected<void, std::string> 已完整写出；失败交中文原因
         */
        [[nodiscard]] std::expected<void, std::string> handOffListener(int peerDescriptor, int listenDescriptor, std::uint64_t targetProcessId) noexcept;

        /// 主动收口（析构也会做）：交棒完成之后不必等对象出作用域就把监听端与套接字文件带走
        void closeChannel() noexcept;

    private:
        explicit UpgradeChannel(Platform::Socket::HandoffChannelEndpoint endpoint) noexcept;

        Platform::Socket::HandoffChannelEndpoint m_endpoint{}; ///< 平台层那条通道的本体
    };

    /**
     * @brief 新一代这一侧：连上交接通道并取回移交来的监听套接字
     * @details 取回的描述符是**已在监听**的，backlog 与已排队的连接一并跟过来，因此接手方直接
     *          用它起服务即可（Net 侧各服务器都有「接管已监听描述符」的构造入口）。
     * @param address UpgradeChannel::address() 给出的地址文本
     * @param connectBudget 连不上时的重试预算：交棒方可能还在准备通道，一次失败不算问题。
     *        预算用完即失败，不无限期等下去
     * @return std::expected<int, std::string> 可直接 accept() 的监听描述符；失败交中文原因
     */
    [[nodiscard]] std::expected<int, std::string> adoptHandedOverListener(std::string_view address, std::chrono::milliseconds connectBudget);
} // namespace AsynGyanis::Core
