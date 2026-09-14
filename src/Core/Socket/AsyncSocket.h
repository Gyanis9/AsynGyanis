/**
 * @file AsyncSocket.h
 * @brief 异步非阻塞 TCP socket — 基于 epoll 边缘触发的协程式 I/O
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 *
 * 封装了 create/bind/listen/connect/receive/send 等 socket 操作,
 * 所有 I/O 方法返回 Task<> 类型, 通过 co_await 实现异步等待。
 * 内部用常驻的 IoWatcher 处理 EAGAIN/EWOULDBLOCK：注册推迟到**第一次等待**时才发生
 * （见 ensureWatcher()），关注位在等待期间按需武装（详见 IoWatcher 的说明）。
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/IoWatcher.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"

#include <memory>
#include <optional>

namespace AsynGyanis::Core
{
    class InetAddress;
    class EventLoop;

    /**
     * @brief 异步 TCP socket 封装，支持协程式 I/O
     *
     * 持有 EventLoop 引用和非阻塞文件描述符，提供协程式异步 I/O 方法。
     * 所有 async* 方法内部通过 while(true) 循环处理 EAGAIN,
     * 在不可用状态时通过常驻注册的 IoWatcher 挂起协程等待文件描述符就绪。
     *
     * @note 支持移动语义，不可复制
     * @note close() 会先调用 shutdown(SHUT_RDWR) 再 close，避免 TCP RST 异常断开
     * @note 所有异步操作均通过 EventLoop 中的 epoll 实例等待事件，不会阻塞线程
     */
    class AsyncSocket
    {
    public:
        /**
         * @brief 从已有文件描述符构造（通常用于 accept 返回的 socket）
         * @param loop           关联的 EventLoop，用于事件注册与等待
         * @param fileDescriptor 已打开且已设置为非阻塞的 socket 文件描述符
         */
        AsyncSocket(EventLoop &loop, int fileDescriptor);

        /**
         * @brief 析构函数，自动调用 close() 关闭 socket
         */
        ~AsyncSocket();

        // 禁止拷贝
        AsyncSocket(const AsyncSocket &) = delete;

        AsyncSocket &operator=(const AsyncSocket &) = delete;

        /**
         * @brief 移动构造函数，转移资源所有权
         * @param other 被移动的 AsyncSocket 对象
         */
        AsyncSocket(AsyncSocket &&other) noexcept;

        /**
         * @brief 移动赋值运算符，转移资源所有权
         * @param other 被移动的 AsyncSocket 对象
         * @return *this
         */
        AsyncSocket &operator=(AsyncSocket &&other) noexcept;

        /**
         * @brief 创建新 socket 的工厂方法
         *
         * 内部调用 socket() 并自动设置 SOCK_NONBLOCK | SOCK_CLOEXEC 标志，
         * 确保 socket 是非阻塞的，且子进程不会继承该文件描述符。
         *
         * @param loop   关联的 EventLoop
         * @param domain 协议族，通常为 AF_INET（IPv4）或 AF_INET6（IPv6）
         * @param type   socket 类型，默认为 SOCK_STREAM（TCP）
         * @return AsyncSocket 实例
         * @throws SystemException 当 socket() 系统调用失败时抛出
         */
        static AsyncSocket create(EventLoop &loop, int domain = AF_INET, int type = SOCK_STREAM);

        /**
         * @brief 绑定 socket 到指定的 sockaddr 地址
         * @param address       指向 sockaddr 结构的指针（IPv4 或 IPv6）
         * @param addressLength 地址结构的长度
         * @return 成功返回 true，失败返回 false（errno 仍可获取）
         */
        bool bind(const sockaddr *address, socklen_t addressLength) const;

        /**
         * @brief 绑定 socket 到 InetAddress 对象表示的地址
         * @param address InetAddress 地址（内含协议族、IP、端口）
         * @return 成功返回 true，失败返回 false
         */
        bool bind(const InetAddress &address) const;

        /**
         * @brief 监听队列的默认长度
         *
         * @details 不用平台的 SOMAXCONN：它的语义是「内核可自行放大队列」，会让连接洪泛时
         *          失去背压；这里给一个明确的默认值，超出部分由内核按各平台策略丢包。
         *          128 与 Linux 早期 somaxconn 的默认值一致，足够覆盖常规突发。
         *          需要更深或更浅的队列时由调用方显式传参。
         */
        static constexpr int kDefaultListenBacklog = 128;

        /**
         * @brief 开始监听 socket（用于服务端）
         * @param backlog 连接等待队列的最大长度，一般传 kDefaultListenBacklog
         * @return 成功返回 true，失败返回 false
         */
        bool listen(int backlog) const;

        /**
         * @brief 异步连接远端服务器（协程式）
         * @param address       远端地址的 sockaddr 指针
         * @param addressLength 地址结构的长度
         * @return Task<> — co_await 等待连接建立完成
         *
         * 使用非阻塞 connect()，如果立即成功则直接返回；如果返回 EINPROGRESS，
         * 则挂起等待 EPOLLOUT 事件，连接完成后恢复。
         * @note 必须在绑定本地地址（可选）之后调用
         */
        Task<> asyncConnect(const sockaddr *address, socklen_t addressLength) const;

        /**
         * @brief 异步连接远端服务器（InetAddress 版本）
         * @param address 远端地址，**按值接收**：本方法是惰性启动的协程，函数体要到首次
         *        resume 才执行，若按引用接收，调用方传临时对象（如
         *        `co_await socket.asyncConnect(InetAddress("127.0.0.1", 8080))`）就会让引用
         *        指向已销毁的对象，而且不报错、只静默读到垃圾地址
         * @return Task<> — co_await 等待连接完成
         * @throws Base::SystemException 连接失败（对端拒绝、超时、地址不可达等）
         */
        Task<> asyncConnect(InetAddress address) const;

        /**
         * @brief 异步接收数据（协程式）
         * @param buffer 接收数据的缓冲区指针
         * @param length 缓冲区最大能接收的字节数，不得超过 INT_MAX（超出会被底层 C API 静默窄化，
         *        因此本方法提前报错，请分段调用）
         * @return Task<ssize_t> — co_await 返回实际接收的字节数；0 表示对端正常关闭连接
         *
         * 内部循环调用 recv(MSG_NOSIGNAL)，当没有数据可读（EAGAIN）时，
         * 挂起等待 EPOLLIN 事件。
         * @note 无数据时不产生 SIGPIPE 信号（MSG_NOSIGNAL）
         * @note **buffer 必须活到本次 co_await 恢复**：协程在挂起期间只持有这个裸指针，
         *       违约（例如把临时缓冲传进来后立刻离开作用域）会让恢复后的 recv 写入已释放内存，
         *       且不报错、表现为随机数据损坏
         * @throws Base::SystemException 接收失败（连接重置等），或 length 超过 INT_MAX；
         *         异常文本带平台 socket 错误码与其可读描述（winsock 失败不写 errno，不要按 errno 判读）
         */
        Task<ssize_t> asyncReceive(void *buffer, size_t length) const;

        /**
         * @brief 异步发送数据（协程式）
         * @param buffer 发送数据的缓冲区指针
         * @param length 待发送的字节数，不得超过 INT_MAX（同 asyncReceive 的理由）
         * @return Task<ssize_t> — co_await 返回实际发送的字节数（可能小于 length）
         *
         * 内部循环调用 send(MSG_NOSIGNAL)，当发送缓冲区满（EAGAIN）时，
         * 挂起等待 EPOLLOUT 事件。
         * @note 返回 -1 表示**对端已关闭连接**（底层 send 返回 0 的那条路径），
         *       这条路径下 errno 未被设置，判定请以返回值为准，不要去读 errno
         * @note **buffer 必须活到本次 co_await 恢复**，理由同 asyncReceive
         * @throws Base::SystemException 发送失败（连接重置等），或 length 超过 INT_MAX
         */
        Task<ssize_t> asyncSend(const void *buffer, size_t length) const;

        /**
         * @brief 聚合发送：一次系统调用提交多段数据，全部发完才返回
         * @details 与 asyncSend 一样吸收「发送缓冲满」并挂起等待可写，区别在于把多段数据
         *          合成一次提交（scatter/gather），省掉「头部块 + 正文」拼进同一块缓冲的那次
         *          整体拷贝——正文越大越明显。部分写由内部游标推进，调用方不必关心。
         * @param buffers 段数组，按序拼接即为要发送的字节流
         * @param bufferCount 段数，必须落在 [1, Platform::Socket::kMaximumVectorCount] 内
         * @return Task<ssize_t> — 全部发送完成时返回总字节数；返回 -1 表示对端已关闭（同 asyncSend）
         * @note **每段的地址必须活到本次 co_await 恢复**，理由同 asyncSend
         * @throws Base::SystemException 段数为 0 或超过平台上限（当场拒绝而不是静默拆分），
         *         或发送失败（连接重置等）
         */
        Task<ssize_t> asyncSendVectored(const Platform::Socket::WriteBuffer *buffers, size_t bufferCount) const;

#if ASYN_PLATFORM_WIN32
        /**
         * @brief 取走监听描述符上由完成端口后端接入的连接（仅 Windows）
         *
         * @details Windows 的接受路径由 AcceptEx 完成：连接到达时它已经被摘下并接入后端的
         *          接受套接字，`::accept()` 看不到它，只能由这里取走。调用方（TcpAcceptor）
         *          的用法是「先取，取不到就 waitReadable() 等一次，再取」。
         * @return 已接入连接的描述符（所有权归调用方）；std::nullopt 表示当前没有待取的连接
         * @note 只对监听描述符有意义；非监听描述符或未注册的描述符一律返回 std::nullopt
         */
        [[nodiscard]] std::optional<int> takeAcceptedConnection();

#endif

#if !ASYN_PLATFORM_WIN32
        /**
         * @brief 异步零拷贝发送：把文件的一段直接推给套接字（Linux sendfile）
         * @details 与 asyncSendVectored 同语义：内部吸收「发送缓冲满」并挂起等待可写，
         *          全部发完才返回。正文不经过用户态缓冲——内核把文件页缓存直接推给协议栈，
         *          省掉整份文件的拷贝与首触缺页，静态文件响应走这条路径最划算。
         * @param fileDescriptor 源文件描述符（须为普通文件，如 MemoryMappedFile::nativeFileDescriptor()）
         * @param offset 从文件的第几个字节开始发送（不动描述符自身的读写偏移）
         * @param length 待发送的字节数
         * @return Task<ssize_t> — 全部发送完成时返回总字节数；返回 -1 表示对端已关闭（同 asyncSend）
         * @note 只有 Linux 与普通 TCP 套接字可用：TLS 记录层不参与（本方法不在 TlsSocket 上），
         *       Windows 没有等价原语，调用方按平台与传输层能力条件编译选用
         * @throws Base::SystemException 源文件描述符非法、待发字节数为 0，或发送失败
         *         （连接重置、对端在发送期间关闭等）
         */
        Task<ssize_t> asyncSendFile(int fileDescriptor, std::uint64_t offset, size_t length) const;
#endif

        /**
         * @brief 关闭 socket（先 shutdown 再 close，避免 TCP RST）
         *
         * 首先调用 shutdown(SHUT_RDWR) 优雅关闭读写通道，
         * 然后调用 close() 释放文件描述符。这样可以避免在未读取完数据时
         * 直接 close 导致对端收到 RST 异常。
         */
        void close();

        /**
         * @brief 获取原始文件描述符
         * @return socket 文件描述符，若已关闭则返回 -1
         */
        [[nodiscard]] int fileDescriptor() const noexcept;

        /**
         * @brief 交出描述符所有权（不搬事件注册）
         *
         * @details 用于把一条刚接受的连接转交给别的循环：调用方拿走描述符后必须立即接管，
         *          本对象此后不再关闭它，也不再认为自己持有连接。
         * @note **只允许在描述符尚未注册事件时调用**（刚 accept、还没读过写过的套接字正是这种状态）：
         *       事件注册只存在于本对象的等待器里，跨循环搬不走，已注册的连接不能用这个方法移交。
         * @return int 交出的描述符；本对象随即变为无效（fileDescriptor() 返回 -1），析构不再关闭它
         */
        [[nodiscard]] int releaseFileDescriptor() noexcept;

        /**
         * @brief 设置 O_NONBLOCK 标志（通常创建时已设置，此处保留以备重新设置）
         */
        void setNonBlocking() const;

        /**
         * @brief 设置 socket 选项（如 SO_REUSEADDR、TCP_NODELAY 等）
         * @param level  协议层（如 SOL_SOCKET、IPPROTO_TCP）
         * @param opt    选项名
         * @param value  选项值的指针
         * @param length 选项值的长度
         * @return 成功返回 true，失败返回 false
         */
        bool setSockOpt(int level, int opt, const void *value, socklen_t length) const;

        /**
         * @brief 获取对端地址（已连接的 socket）
         * @return InetAddress 对象，包含对端的 IP 和端口
         * @throws Base::SystemException 如果获取失败（如未连接或 socket 无效）
         */
        InetAddress remoteAddress() const;

        /**
         * @brief 获取本地地址（绑定的地址）
         * @return InetAddress 对象，包含本地的 IP 和端口
         * @throws Base::SystemException 如果获取失败
         */
        InetAddress localAddress() const;

        /**
         * @brief 等待套接字可读（EPOLLIN）
         * @details 低层就绪等待，供 I/O 循环与封装层（如 TlsSocket）使用：它复用本套接字
         *          已经常驻注册的 epoll 注册对象，因此不额外产生 epoll_ctl，也不额外分配协程帧。
         *          一般业务代码直接用 asyncReceive()/asyncSend() 即可。
         * @return IoWatcher::Awaiter 等待器，可直接 co_await
         * @throws Base::SystemException 套接字无效或已关闭
         */
        [[nodiscard]] IoWatcher::Awaiter waitReadable() const;

        /**
         * @brief 等待套接字可写（EPOLLOUT）
         * @details 语义同 waitReadable()，方向为可写。
         * @return IoWatcher::Awaiter 等待器，可直接 co_await
         * @throws Base::SystemException 套接字无效或已关闭
         */
        [[nodiscard]] IoWatcher::Awaiter waitWritable() const;

    private:
        EventLoop &m_loop;           ///< 关联的事件循环，用于异步等待和事件注册
        int        m_fileDescriptor; ///< 底层 socket 文件描述符，-1 表示无效

        /**
         * @brief 按需创建常驻注册对象（第一次等待时调用）
         *
         * @details 注册推迟到第一次等待才做，有两个必须的理由：
         *          - **接受分发**：连接在监听循环上被接受、随即把描述符移交给工作循环。
         *            epoll 允许同一个描述符出现在多个实例里，而 Windows 的完成端口**绑过一次就
         *            换不了端口**——在监听循环上注册过的描述符，到工作循环里再也注册不上。
         *            不在构造时注册，移交时才不会带上错误的归属。
         *          - 只是收发系统调用就完成的套接字（多数短连接、以及被移交的那些）不必为
         *            「可能永远用不到的注册」付一次系统调用。
         *
         * @return IoWatcher* 注册对象；描述符无效或已关闭时为空指针
         * @throws Base::SystemException 注册失败（同一描述符已被另一个注册对象占用、描述符非法）
         */
        [[nodiscard]] IoWatcher *ensureWatcher() const;

        /**
         * @brief 常驻 epoll 注册（等待时按方向武装），第一次等待时创建
         * @details 堆分配而非直接持有：epoll 里记的是注册对象的**地址**，而本类是可移动的
         *          （移动后描述符跟着走）。直接持有成员会在移动时改变地址，让 epoll 里的
         *          用户数据悬空；堆对象随指针转移，地址始终不变。
         *          可读与可写共用一个注册对象：同步只允许一个方向有等待者，方向由各等待器指定。
         *          `mutable`：等待方法本身是 const，但「第一次等待」要把注册对象建出来
         */
        mutable std::unique_ptr<IoWatcher> m_watcher;
    };
} // namespace AsynGyanis::Core
