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
     * @brief 一对套接字地址：本体与长度
     *
     * @details 长度随地址族变化，因此两者必须一起传递，不能各自单独存。数据报路径尤其需要它：
     *          同一条数据报套接字要面对任意多个对端，收发都得把「这一条是对谁/来自谁」带上。
     * @note 与 Core::InetAddress 的分工：本类型是平台层的裸结构，只负责在系统调用之间搬运；
     *       IP 文本化、端口访问、解析等便利操作在 Core 层做。
     */
    struct SocketAddress
    {
        sockaddr_storage storage{}; ///< 地址本体（放得下 IPv4/IPv6）
        socklen_t        length{0}; ///< 实际长度；0 表示未设置
    };

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
            Initialization() noexcept : m_valid(initialize())
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
         * @details Linux 使用 accept4 一次性置入非阻塞与 close-on-exec 标志；Windows 无 accept4，
         *          接受成功后单独设置非阻塞并取消句柄继承位——两侧交出的连接都不得随进程创建传下去。
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
         * @brief 在监听套接字上开启 TCP Fast Open（TFO）
         * @details TFO 允许客户端在三次握手完成之前就携带数据（RFC 7413）：客户端把首个数据段
         *          放进 SYN，服务端验证 cookie 后即可连同握手应答一起交付给应用层，省掉一个 RTT。
         *          内核在服务端只接受携带有效 TFO cookie 的连接，普通连接不受影响，因此开启本项
         *          对既有客户端是透明的。
         * @param descriptor 目标监听套接字描述符
         * @param queueLength 允许同时处于「TFO 未完成握手」状态的连接数上限；0 表示关闭 TFO。
         *        负值没有对应语义，直接拒绝
         * @return true 设置成功；false 取值非法、平台不提供该选项（内核或 SDK 头里没有
         *         TCP_FASTOPEN）或设置失败
         * @note Windows 与 Linux 都提供该套接字选项，但两侧对「读回值」的约定不同：
         *       Linux 原样回读入参，Windows 只回读 1/0（是否开启），因此读回校验不能按入参比对
         * @note 开启只是「允许」：服务端是否真正接受 TFO 由系统开关（Linux 的
         *       net.ipv4.tcp_fastopen 服务端位）决定，两处都就位时才生效
         */
        static bool setFastOpen(int descriptor, int queueLength) noexcept;

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
         * @note 返回 0 表示源侧已到文件末尾（调用方传的 length 恒大于 0），即文件在发送期间被
         *       截断：与返回 -1 的「可重试」语义完全不同，调用方必须分开处理，否则续发循环会
         *       按 kWouldBlock 原地空转成死循环
         * @note SIGPIPE 由 Socket::initialize() 在初始化时忽略，本函数不必带 MSG_NOSIGNAL
         *       这类标志（sendfile 也没有对应标志）
         * @note 只有 Linux 提供：Windows 的 TransmitFile 在本引擎里不可用——实测（非阻塞套接字、
         *       对端只读 8 MiB 文件）首次调用就把线程阻塞住，直到对端把数据收完；且它一次调用
         *       只推进 32768 字节后仍返回成功、文件指针不再前进，进度无法反推。要用它必须走
         *       重叠 I/O 并把完成事件接到完成端口上，而那需要 IOCP 后端支持用户发起的异步操作。
         *       在此之前 Windows 静态文件仍走聚合写（非阻塞、正确，只是多一次用户态拷贝）
         */
        static ssize_t sendFileChunk(int socketDescriptor, int fileDescriptor, std::uint64_t offset, std::size_t length) noexcept;
#endif

        // ============================================================================
        // 跨进程移交监听套接字：零停机换代的地基
        // ============================================================================

        /**
         * @brief 通过一条已连通的字节通道，把一个监听套接字交给另一个进程
         *
         * @details 两个平台走各自的系统机制，线路格式统一成「定长头 + 平台特定的载体」，
         *          因此两侧的收发必须配对本层的这一对函数：
         *          @li Windows 用 WSADuplicateSocketW 向目标进程换一份 WSAPROTOCOL_INFO，把这张
         *              协议信息作为载荷写出——套接字句柄本身不会随 spawn 继承，这是本机上唯一
         *              能把监听态交给别的进程的路子；
         *          @li POSIX 用 sendmsg 的 SCM_RIGHTS 控制消息直接把描述符送过去，头里只带地址族
         *              与类型（内核会在目标进程里重装这个描述符）。
         *          头一份都收不全（通道被关）与平台不支持都按失败报告，不返回「半个套接字」。
         * @param channelDescriptor 已连通的通道套接字（本函数按阻塞语义收发完整一条消息）
         * @param listenDescriptor 要移交的监听套接字；必须已经 listen() 过
         * @param targetProcessId 接收方进程号（Windows 按进程号认目标；POSIX 上内核自己处理）
         * @return true 已完整写出
         * @return false 平台不支持、参数非法或通道写坏，原因见 PlatformError::lastErrorCode()
         * @note **POSIX 上通道必须是 AF_UNIX 流套接字**。内核只在 unix 域里随 SCM_RIGHTS 送描述符：
         *       同一段代码走 loopback TCP 时 sendmsg 与 recvmsg 都返回成功、8 字节数据一字不差，
         *       只有控制消息被静默丢掉（实测 recvmsg 后 msg_controllen 归 0），接收侧于是只拿得到
         *       「一个描述符也没有」。实测对照：AF_UNIX 通道 recvmsg 后 controllen=24 且能拿到可用
         *       描述符，TCP 通道 controllen=0。Windows 侧载荷是普通字节，任何字节流通道都行。
         * @note 通道本身不带鉴权：本层不校验「对面就是我要交给的那个进程」，那是调用方建通道时的
         *       责任（POSIX 上 unix 域套接字的文件权限就是那道门，Windows 上 loopback 即本机）
         */
        static bool writeListeningSocketHandoff(int channelDescriptor, int listenDescriptor, std::uint64_t targetProcessId) noexcept;

        /**
         * @brief 从通道里收下对端移交来的监听套接字
         * @details 与 writeListeningSocketHandoff 成对：读出头与载体，在本进程里重建一个可直接
         *          accept() 的监听套接字。Windows 走 WSASocketW(FROM_PROTOCOL_INFO)，
         *          POSIX 直接取 SCM_RIGHTS 里重装好的描述符。
         * @param channelDescriptor 已连通的通道套接字（按阻塞语义收完整一条消息；POSIX 上必须是
         *        AF_UNIX 流套接字，见 writeListeningSocketHandoff 的那条 @note）
         * @return int 新描述符（监听态与 backlog 都跟着过来，已排队连接也一并继承）；失败返回 -1
         *         并置错误码——不会返回「半个套接字」
         * @note 错误码的读法：EINVAL 表示「收到的不像本平台的移交消息」（长度不对或载荷被截断），
         *       EBADF 表示「字节收齐了但里面没有描述符」——POSIX 上这一条几乎都是通道用错了类型
         * @note 换代的关键性质在这里：本端 accept 到的是**上一代进程还在服务时**就已排队的连接，
         *       因此新进程接手期间监听端口不曾关闭，也就没有 ECONNREFUSED 的空窗
         */
        static int readListeningSocketHandoff(int channelDescriptor) noexcept;
    };
} // namespace AsynGyanis::Platform
