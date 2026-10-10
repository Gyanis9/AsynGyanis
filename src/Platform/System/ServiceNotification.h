/**
 * @file ServiceNotification.h
 * @brief 服务管理器的状态通知：把 READY / RELOADING / STOPPING 送进 $NOTIFY_SOCKET 那个数据报套接字
 * @author Gyanis
 * @date 2026-10-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Platform
{
    /**
     * @brief 服务管理器（systemd 一类的监督者）的状态通知出口
     *
     * @details 本类只做一件事：按 sd_notify(3) 的形状，把一段「换行分隔的赋值」作为**一个数据报**
     *          交给 `$NOTIFY_SOCKET`。地址的三种前缀按那份文档处理：首字符 `/` 是文件系统上的
     *          AF_UNIX 套接字，`@` 是 Linux 抽象命名空间，`vsock:` 是 AF_VSOCK。前两种本类支持，
     *          vsock 明确拒绝并说明原因——把它当路径去连一个不存在的文件名，报出来的会是
     *          「没有那个文件或目录」，而真正的原因是这条通路本层没接（把「发不出去」写成
     *          「发不出去的原因是别的」是最难归因的一类运维缺陷）。
     * @details 为什么落在 Platform：这条路上只有一次 `sendto` 与两次环境变量读取，本仓的分工是
     *          系统调用只进这一层。**什么时候**通知谁由调用方决定（启动确认之后、重读配置之前、
     *          收尾一开始），那部分编排属于运行时，不在这里。
     * @details 不在服务管理器下运行（开发机、容器、sysvinit）时 `$NOTIFY_SOCKET` 根本不存在，
     *          `open()` 因此失败并给出「未配置」这句原因；这是文档认可的正常形态（那份写明没有
     *          NOTIFY_SOCKET 就没有状态可发），调用方可以据此打一行「本进程未受服务管理器监督」，
     *          而不必把一次失败当成缺陷。
     * @note Windows 侧没有这套约定：所有入口都失败并给出同一句平台事实，不做静默成功——
     *       静默成功的后果是部署方以为通知发出去了，而 systemd 那侧的 Type=notify 永远等不到
     *       READY=1，这种缺陷要到第一次真上线才暴露。
     * @note 本类不碰看门狗（`WATCHDOG_USEC` / `WATCHDOG=1`）。那条节拍必须由事件循环 own 着发，
     *       才配得上它的作用——「主线程还活着而循环卡死」正是要被重启的那种状态；在没有接上
     *       循环内节拍之前读到 `WATCHDOG_USEC` 却不发 `WATCHDOG=1`，等于让监督者按一个没人喂的
     *       超时把进程杀掉。所以这里刻意不读它。
     */
    class ASYN_PLATFORM_API ServiceNotification
    {
    public:
        /// 监听器已经就绪、可以接流量（`Type=notify` 的服务单元靠这一条判定启动完成）
        static constexpr std::string_view kReadyState = "READY=1";

        /// 正在收尾：监督者从这一刻起开始计算停机超时
        static constexpr std::string_view kStoppingState = "STOPPING=1";

        /**
         * @brief 拼一条 `STATUS=` 状态文本
         * @details 文档要求 STATUS 是**单行**文本：串里出现换行会被对端当成下一条赋值，
         *          于是「一条状态」变成「两条状态」，而其中一条还是没人认识的键。这里把 CR、LF、
         *          Tab 与其它不可打印字符一律换成空格，可打印文本（含中文）原样保留。
         * @param text 状态描述，例如 "正在重读配置"
         * @return std::string 形如 "STATUS=正在重读配置" 的赋值串
         */
        [[nodiscard]] static std::string statusState(std::string_view text);

        ServiceNotification() = default;

        /**
         * @brief 析构时关掉套接字（若还开着）
         */
        ~ServiceNotification();

        ServiceNotification(const ServiceNotification &)            = delete;
        ServiceNotification &operator=(const ServiceNotification &) = delete;
        ServiceNotification(ServiceNotification &&)                 = delete;
        ServiceNotification &operator=(ServiceNotification &&)      = delete;

        /**
         * @brief 读 `$NOTIFY_SOCKET` 并建好数据报套接字
         * @return true 已可发送；false 表示未配置、地址形状本层不支持或系统调用失败，原因见 lastError()
         */
        bool open();

        /**
         * @brief 套接字是否已备好
         * @return true 已 open() 且尚未 close()
         */
        [[nodiscard]] bool isOpen() const noexcept;

        /**
         * @brief 交出一段状态
         * @details 只发一次，不重试：这是 UDP 语义的数据报，交没交到由监督者自己的超时决定后果，
         *          本层替它重试只会把「这条状态发过几次」这笔账弄浑。
         * @param state 换行分隔的赋值串；末尾换行由对端补齐（文档：一个结尾换行是隐含的），
         *              空串在发送之前就被拒
         * @return true 数据报已交出；false 表示未打开或发送失败，原因见 lastError()
         */
        bool send(std::string_view state) noexcept;

        /**
         * @brief 主动关掉套接字（析构会做同一件事，重复调用是空操作）
         */
        void close() noexcept;

        /**
         * @brief 最近一次失败的原因
         * @details 与基类那批驱动的约定一致：成功路径上会被清空，所以「非空」等价于「本次失败」。
         * @return const std::string & 中文原因，带上系统错误码时以「（错误码 N）」结尾
         */
        [[nodiscard]] const std::string &lastError() const noexcept;

    private:
        /**
         * @brief 把 `$NOTIFY_SOCKET` 的文本折成对端地址
         * @param addressText 环境变量原文
         * @return true 折好了（写进 m_destination 与 m_destinationLength）；
         *             false 表示形状不支持或过长，原因写进 m_lastError
         */
        bool buildDestination(const std::string &addressText) noexcept;

        int m_descriptor{-1}; ///< 数据报套接字描述符；-1 表示尚未打开
        /// 对端地址的原始字节（`sockaddr_un`，含抽象命名空间那种带 NUL 首字节的形状）；
        /// 头文件因此不必暴露 <sys/un.h>
        std::vector<char> m_destination{};
        std::size_t       m_destinationLength{0}; ///< 交给 sendto 的地址长度，抽象命名空间下不含结尾 NUL
        std::string       m_lastError;            ///< 最近一次失败原因
    };
} // namespace AsynGyanis::Platform
