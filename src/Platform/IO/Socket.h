/**
 * @file Socket.h
 * @brief Winsock 生命周期管理与 socket 级跨平台原语
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/Platform.h"

#include <cstdint>

namespace AsynGyanis::Platform
{
    /**
     * @brief socket 层跨平台工具
     *
     * @details Windows 上任何 socket API 调用前必须完成 WSAStartup，Linux 上
     *          相应接口为空操作，因此调用方无需平台分支。
     * @note initialize()/finalize() 以引用计数配对：多个持有网络资源的对象可各自成对调用，
     *       Winsock 只在首个 initialize() 时启动、在最后一个 finalize() 时清理。
     */
    class Socket
    {
    public:
        /**
         * @brief 聚合写的一段数据
         * @details 多段按数组顺序拼起来就是线上字节流；地址与长度都必须由调用方保证有效，
         *          本层不持有也不复制数据。
         */
        struct WriteBuffer
        {
            const void *data;   ///< 段起始地址
            std::size_t length; ///< 段字节数
        };

        /**
         * @brief 单次聚合写的段数上限
         * @details Windows 的 WSASend 上限为 16，Linux 的 writev 为 IOV_MAX（1024），
         *          这里取两个平台的公共安全值。超出上限直接失败并置错误码，不静默拆分：
         *          拆分会让「一次系统调用」这个前提悄悄失效，调用方无从察觉。
         */
        static constexpr std::size_t kMaximumVectorCount = 16;

        /**
         * @brief Winsock 初始化引用的 RAII 守卫
         *
         * @details 构造时申请一次 initialize() 引用，析构时自动释放。适用于存在多条
         *          返回路径、手工配对 finalize() 容易遗漏的调用方（如域名解析）。
         */
        class Initialization
        {
        public:
            /**
             * @brief 申请一次网络子系统初始化引用
             */
            Initialization() noexcept :
                m_valid(initialize())
            {
            }

            ~Initialization() noexcept
            {
                // 仅在确实取得引用时释放，避免把引用计数减成负数
                if (m_valid)
                {
                    finalize();
                }
            }

            Initialization(const Initialization &) = delete;

            Initialization &operator=(const Initialization &) = delete;

            /**
             * @brief 查询初始化引用是否申请成功
             * @return true 引用已建立，可继续调用 socket API
             */
            [[nodiscard]] bool isValid() const noexcept
            {
                return m_valid;
            }

        private:
            bool m_valid = false; ///< 是否成功取得 Winsock 初始化引用
        };

        /**
         * @brief 申请网络子系统初始化引用（Windows 下按需执行 WSAStartup）
         * @return true 引用已建立（首次调用会真正启动 Winsock）
         * @return false Windows 下 WSAStartup 失败
         */
        static bool initialize() noexcept;

        /**
         * @brief 释放一次网络子系统初始化引用（引用归零时执行 WSACleanup）
         * @details 与 initialize() 成对调用；仍有其他引用存活时不会真正清理，
         *          以免提前拆掉存活 socket 依赖的 Winsock 状态。
         */
        static void finalize() noexcept;

        /**
         * @brief 接受一条传入连接
         * @details Linux 使用 accept4 一次性置入非阻塞与 close-on-exec 标志；
         *          Windows 无 accept4，接受成功后单独设置非阻塞。
         * @param listenDescriptor 监听描述符
         * @param address 输出参数，对端地址，可为 nullptr
         * @param addressLength 输入输出参数，address 缓冲区容量与实际写入长度
         * @return int 新连接描述符，失败返回 FileDescriptor::kInvalid
         */
        static int accept(int listenDescriptor, sockaddr *address, socklen_t *addressLength) noexcept;

        /**
         * @brief 开启地址复用（SO_REUSEADDR）
         * @details 允许绑定到仍处于 TIME_WAIT 的地址，服务重启时不再偶发「地址已被占用」；
         *          两个平台的语义一致，故监听套接字默认都该打开它。
         * @param descriptor 目标套接字描述符
         * @return true 设置成功；false 失败，可用 PlatformError::lastSocketErrorCode() 取原因
         */
        static bool setReuseAddress(int descriptor) noexcept;

        /**
         * @brief 开启端口复用（SO_REUSEPORT）
         * @details Linux 3.9+ 支持，可让多个监听套接字分摊 accept 队列并各自独立绑定同端口；
         *          Windows 没有该选项，此时返回 false，调用方按「不支持」降级而不必视为错误。
         * @param descriptor 目标套接字描述符
         * @return true 设置成功；false 平台不提供该选项或设置失败
         */
        static bool setReusePort(int descriptor) noexcept;

        /**
         * @brief 关闭 Nagle 算法（TCP_NODELAY）
         * @details 低延迟协议（HTTP 小响应、RPC）必须关闭合批，否则小包会被攒到 ACK 才发出。
         * @param descriptor 目标套接字描述符
         * @return true 设置成功
         */
        static bool setNoDelay(int descriptor) noexcept;

        /**
         * @brief 设置发送缓冲上限（SO_SNDBUF）
         * @details 上限偏小会限制单连接的带宽时延积（高延迟链路上吞吐下降），偏大则在高并发下
         *          按连接放大内存占用。取值只是上限提示：内核会按自身策略取整（Linux 的实际值
         *          约为请求值的两倍，以 getsockopt 读数为准）。
         * @param descriptor 目标套接字描述符
         * @param byteCount 期望的字节数，必须为正
         * @return true 设置成功；false 取值非正或系统拒绝
         */
        static bool setSendBufferSize(int descriptor, int byteCount) noexcept;

        /**
         * @brief 设置接收缓冲上限（SO_RCVBUF），语义同 setSendBufferSize()
         * @param descriptor 目标套接字描述符
         * @param byteCount 期望的字节数，必须为正
         * @return true 设置成功；false 取值非正或系统拒绝
         */
        static bool setReceiveBufferSize(int descriptor, int byteCount) noexcept;

        /**
         * @brief 开启延迟接受（TCP_DEFER_ACCEPT，仅 Linux）
         * @details 内核等到连接上出现数据（或超过给定秒数）才把连接放进 accept 队列，
         *          用于过滤「连上就静默」的空连接并减少事件循环唤醒；Windows 没有该选项，
         *          返回 false，调用方按「不支持」降级而不是当作失败（与 setReusePort() 同惯例）。
         * @param descriptor 目标监听套接字描述符
         * @param seconds 最长等待秒数；0 表示关闭
         * @return true 设置成功；false 平台不提供该选项或设置失败
         */
        static bool setDeferAccept(int descriptor, int seconds) noexcept;

        /**
         * @brief 设置 IPv6 套接字是否只接受 IPv6 连接（IPV6_V6ONLY）
         * @details 双栈监听（isOnlyV6=false）才能同时接住 IPv4 映射地址；
         *          非 IPv6 套接字调用本函数会失败，调用方应先判 family。
         * @param descriptor 目标套接字描述符
         * @param isOnlyV6 true 表示仅接受 IPv6，false 表示双栈
         * @return true 设置成功
         */
        static bool setIpv6Only(int descriptor, bool isOnlyV6) noexcept;

        /**
         * @brief 取出并清除套接字上挂起的错误码（SO_ERROR）
         * @details 非阻塞 connect 返回「进行中」之后，成败只能靠该选项判定；
         *          读取动作本身会清除挂起的错误状态，因此一次连接只应调用一次。
         * @param descriptor 目标套接字描述符
         * @return int 挂起的错误码，0 表示没有错误（连接已建立）
         */
        static int takePendingError(int descriptor) noexcept;

        /**
         * @brief 聚合写：一次系统调用提交多段数据（scatter/gather）
         * @details 典型用途是「头部块 + 正文」这类本来要拼进同一块缓冲再发的数据，分段提交
         *          可以省掉正文那次整体拷贝（大正文/文件响应最明显）。Linux 走 sendmsg
         *          （带 MSG_NOSIGNAL，避免对端已关闭时触发 SIGPIPE），Windows 走 WSASend。
         * @param descriptor 目标套接字描述符
         * @param buffers 段数组，按序拼接即为要发送的字节流
         * @param bufferCount 段数，必须落在 [1, kMaximumVectorCount] 内
         * @return ssize_t 实际写入的字节数；非阻塞套接字在缓冲区满时返回 -1 并置
         *         kWouldBlock（调用方应等可写后重试），对端已关闭按平台语义返回 0 或 -1；
         *         参数非法时返回 -1 并置 kInvalidArgument
         * @note 返回值为正但小于总长度是正常情形（部分写），调用方必须按游标推进剩余部分
         */
        static ssize_t writeVectored(int descriptor, const WriteBuffer *buffers, std::size_t bufferCount) noexcept;

#if !ASYN_PLATFORM_WIN32
        /**
         * @brief 单次零拷贝发送的长度上限
         * @details Linux 内核单次 sendfile 最多搬运 MAX_RW_COUNT（约 2 GiB 减一页），超限直接
         *          EINVAL 而不是部分写入，因此本层在调用前先钳制到这个安全值；调用方按返回的
         *          字节数推进偏移即可，不必关心这个平台上限。
         */
        static constexpr std::size_t kMaximumSendFileChunk = 0x7ffff000;

        /**
         * @brief 零拷贝发送：把文件的一段直接写进套接字（Linux sendfile）
         * @details 正文不经过用户态缓冲：内核把文件页缓存直接推给协议栈，省掉整份文件的
         *          用户态拷贝与映射首触缺页，静态文件响应走这条路径最划算。前提是正文能给出
         *          一个打开的文件描述符（如 MemoryMappedFile::nativeFileDescriptor()）。
         * @param socketDescriptor 目标套接字描述符（须为非阻塞）
         * @param fileDescriptor 源文件描述符（须为普通文件）
         * @param offset 从文件的第几个字节开始发送
         * @param length 期望发送的字节数，内部按 kMaximumSendFileChunk 钳制
         * @return ssize_t 实际写入的字节数；非阻塞套接字在缓冲区满时返回 -1 并置 kWouldBlock
         *         （调用方应等可写后按新偏移重试）；参数非法时返回 -1 并置 kInvalidArgument；
         *         该文件系统不支持零拷贝发送时返回 -1 并置系统错误码
         * @note 只发起一次系统调用，部分写由调用方按「offset 加上返回值」推进。源文件的读写
         *       偏移不受影响（本层显式传偏移指针，不动描述符自身的偏移），同一个文件可被多条
         *       响应并发发送
         * @note SIGPIPE 由 Socket::initialize() 在初始化时忽略，本函数不必带 MSG_NOSIGNAL
         *       这类标志（sendfile 也没有对应标志）
         * @note 只有 Linux 提供：Windows 没有等价原语（TransmitFile 的语义与返回值约定都不同，
         *       接入要等 IOCP 事件后端的决定落地），故不声明，调用方按平台条件编译选用
         */
        static ssize_t sendFileChunk(int socketDescriptor, int fileDescriptor, std::uint64_t offset, std::size_t length) noexcept;
#endif
    };
} // namespace AsynGyanis::Platform
