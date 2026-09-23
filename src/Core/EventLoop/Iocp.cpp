#include "Core/EventLoop/Iocp.h"

#include "Base/Exception/SystemException.h"
#include "Base/Log/LogMacros.h"

#include <algorithm>
#include <cstring>

namespace AsynGyanis::Core
{
    namespace
    {
        /// AcceptEx 的本地/对端地址缓冲：两端各预留 sockaddr_in6 加 16 字节（Microsoft 文档给出的算法）
        constexpr std::size_t kAcceptAddressBufferByteCount = 2 * (sizeof(sockaddr_in6) + 16);

        /// 把 int 描述符还原成 HANDLE（CancelIoEx 等接受 HANDLE 的接口用）
        HANDLE toHandle(const SOCKET socketHandle) noexcept
        {
            return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(socketHandle));
        }

        /**
         * @brief 算一个合并表键落在哪个槽位
         * @param userData 注册对象的用户数据（OVERLAPPED_ENTRY::lpOverlapped）
         * @param slotMask 槽位数 - 1
         * @return std::size_t 起始槽位
         * @details 不直接用 std::hash<void*>（多数实现就是恒等），而是先右移四位再乘 64 位黄金比例
         *          常数：同一批完成通知里各注册对象的地址往往只差固定步长，不把高位打散就会在探测链上
         *          排队（指针按 16 字节对齐，低四位本来就没有信息量，右移不会丢键的区分度）
         */
        std::size_t hashResultSlotKey(void *const userData, const std::size_t slotMask) noexcept
        {
            constexpr std::uint64_t kGoldenRatioOddConstant = 0x9E3779B97F4A7C15ULL;
            const auto              keyBits = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(userData)) >> 4U;
            return static_cast<std::size_t>(keyBits * kGoldenRatioOddConstant) & slotMask;
        }

        /**
         * @brief 取 AcceptEx 的函数指针
         * @param probeSocket 任意有效的套接字（Winsock 按 GUID 问一次即可，进程内缓存）
         * @return LPFN_ACCEPTEX 函数指针；本机不支持时为 nullptr
         */
        LPFN_ACCEPTEX resolveAcceptExFunction(const SOCKET probeSocket) noexcept
        {
            static const LPFN_ACCEPTEX function = [probeSocket]() -> LPFN_ACCEPTEX
            {
                GUID         extensionGuid = WSAID_ACCEPTEX;
                LPFN_ACCEPTEX pointer      = nullptr;
                DWORD        returnedByteCount = 0;
                if (::WSAIoctl(probeSocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &extensionGuid, sizeof(extensionGuid), &pointer,
                               sizeof(pointer), &returnedByteCount, nullptr, nullptr) != 0)
                {
                    return nullptr;
                }
                return pointer;
            }();
            return function;
        }
    } // namespace

    struct Iocp::ProbeContext
    {
        OVERLAPPED    overlapped{};               ///< 必须是第一个成员：完成通知给出的就是它的地址
        SocketState  *owner{nullptr};             ///< 所属的套接字状态
        std::uint32_t direction{0};               ///< EPOLLIN（读/AcceptEx 探针）或 EPOLLOUT（写探针）
    };

    struct Iocp::SocketState
    {
        SOCKET        socketHandle{INVALID_SOCKET};   ///< 被注册的套接字
        void         *userData{nullptr};              ///< 上报事件时写进 epoll_event.data.ptr
        std::uint32_t registeredEvents{0};            ///< 最近一次登记的关注位（含 EPOLLONESHOT 与否）
        ProbeContext  readProbe{};                    ///< 读方向（监听描述符上是 AcceptEx）
        ProbeContext  writeProbe{};                   ///< 写方向
        char          readBuffer[1]{};                ///< 1 字节 MSG_PEEK 探针缓冲：只读不取，内容无用
        bool          hasReadProbe{false};            ///< 读探针是否已在途
        bool          hasWriteProbe{false};           ///< 写探针是否已在途
        bool          isListening{false};             ///< 该套接字是否处于监听态（决定读探针用 AcceptEx）

        /// 是否数据报套接字（SOCK_DGRAM）：-1 未知、0 否、1 是。数据报无连接，read 探针的
        /// 「先确认已连上」守卫对它必然不成立，而 WSARecv 在无连接 UDP 上本就合法
        int isDatagramSocket{-1};

        /// 「连接性已确认」：-1 未知、1 已确认。已连上的流式套接字与数据报套接字都属于这一类，
        /// 此后每次武装都可以跳过 SO_ACCEPTCONN 与 getpeername 两次探测（各一次内核往返）
        int isConnectivitySettled{-1};

        /// 探针投递时就撞上硬错误（对端复位、套接字已失效）的方向位：没有完成通知可等，
        /// 由 wait() 合成一条错误事件交给等待方，见 harvestSyntheticErrorEvents()
        std::uint32_t readyDirections{0};

        /// 是否已排进待合成表（与 isArmRetryQueued 同一套路，避免同一个状态重复入表）
        bool isSyntheticReadyQueued{false};

        bool          isDeleted{false};               ///< 已注销但仍有完成通知在队，见 m_graveyard
        std::uint32_t failedDirections{0};            ///< 上一次投递失败的方向位（等下一次 wait() 重试）
        bool          isArmRetryQueued{false};        ///< 是否已排进待重试表（避免重复入表）
        SOCKET        pendingAcceptSocket{INVALID_SOCKET}; ///< AcceptEx 正在使用的接受套接字
        SOCKET        acceptedSocket{INVALID_SOCKET};      ///< 已接入、等 takeAcceptedSocket() 取走
        bool          hasAcceptedSocket{false};            ///< acceptedSocket 是否有效
        /// AcceptEx 的地址输出缓冲（约 112 字节）：只有监听套接字用得上，
        /// 因此首次投递 AcceptEx 时才分配——普通连接不为它付出常驻内存
        std::unique_ptr<char[]> acceptAddressBuffer;

        /// 是否还有探针没等到完成通知
        [[nodiscard]] bool hasProbeInFlight() const noexcept
        {
            return hasReadProbe || hasWriteProbe;
        }

        /// 初始化探针的固定字段
        void initProbes(void *const watcherData) noexcept
        {
            readProbe.owner     = this;
            readProbe.direction = EPOLLIN;
            writeProbe.owner     = this;
            writeProbe.direction = EPOLLOUT;
            userData            = watcherData;
        }
    };

    Iocp::Iocp()
    {
        // 第一个参数传 INVALID_HANDLE_VALUE 表示「只创建完成端口、不关联任何文件」
        m_iocp = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
        if (m_iocp == nullptr)
        {
            // 必须显式传 Win32 空间的码：CreateIoCompletionPort 失败只写 GetLastError，不写 errno，
            // 走隐式 errno 通道会报出一个与本次失败无关（或干脆是 0）的描述
            throw Base::SystemException("创建完成端口失败（CreateIoCompletionPort）",
                                        std::error_code(static_cast<int>(::GetLastError()), std::system_category()));
        }
        m_entries.resize(kMaximumEventCount);
        m_results.reserve(kMaximumEventCount);
        m_resultSlotUserData.resize(kInitialResultMergeSlotCount);
        m_resultSlotIndex.assign(kInitialResultMergeSlotCount, kEmptyResultSlot);
        m_usedResultSlots.reserve(kInitialResultMergeSlotCount);
        m_resultSlotMask = kInitialResultMergeSlotCount - 1;
    }

    Iocp::~Iocp()
    {
        destroy();
    }

    Iocp::Iocp(Iocp &&other) noexcept :
        m_iocp(other.m_iocp),
        m_entries(std::move(other.m_entries)),
        m_results(std::move(other.m_results)),
        m_resultSlotUserData(std::move(other.m_resultSlotUserData)),
        m_resultSlotIndex(std::move(other.m_resultSlotIndex)),
        m_usedResultSlots(std::move(other.m_usedResultSlots)),
        m_resultSlotMask(other.m_resultSlotMask),
        m_sockets(std::move(other.m_sockets)),
        m_graveyard(std::move(other.m_graveyard)),
        m_pendingRearm(std::move(other.m_pendingRearm)),
        m_pendingArmRetry(std::move(other.m_pendingArmRetry)),
        m_pendingSyntheticReady(std::move(other.m_pendingSyntheticReady))
    {
        other.m_iocp = nullptr;
    }

    Iocp &Iocp::operator=(Iocp &&other) noexcept
    {
        if (this != &other)
        {
            destroy();
            m_iocp         = other.m_iocp;
            m_entries      = std::move(other.m_entries);
            m_results      = std::move(other.m_results);
            m_resultSlotUserData = std::move(other.m_resultSlotUserData);
            m_resultSlotIndex    = std::move(other.m_resultSlotIndex);
            m_usedResultSlots    = std::move(other.m_usedResultSlots);
            m_resultSlotMask     = other.m_resultSlotMask;
            m_sockets         = std::move(other.m_sockets);
            m_graveyard       = std::move(other.m_graveyard);
            m_pendingRearm    = std::move(other.m_pendingRearm);
            m_pendingArmRetry = std::move(other.m_pendingArmRetry);
            m_pendingSyntheticReady = std::move(other.m_pendingSyntheticReady);
            other.m_iocp      = nullptr;
        }
        return *this;
    }

    void Iocp::destroy()
    {
        // 先取消在途探针：CancelIoEx 只发起取消，每条探针仍会以 ERROR_OPERATION_ABORTED
        // 完成并入队；完成时内核还要写 OVERLAPPED 里的状态码，因此通知必须收完
        for (auto &[fileDescriptor, state]: m_sockets)
        {
            static_cast<void>(fileDescriptor);
            cancelProbes(*state);
        }
        for (SocketState *state: m_graveyard)
        {
            cancelProbes(*state);
        }

        // 排空完成队列再释放状态：不排空就 delete，取消才完成的探针会把状态码
        // 写进已释放的 OVERLAPPED
        drainCompletions();

        if (m_iocp != nullptr)
        {
            ::CloseHandle(m_iocp);
            m_iocp = nullptr;
        }
        for (auto &[fileDescriptor, state]: m_sockets)
        {
            static_cast<void>(fileDescriptor);
            closeAcceptedSockets(*state);
            delete state;
        }
        m_sockets.clear();
        for (SocketState *state: m_graveyard)
        {
            closeAcceptedSockets(*state);
            delete state;
        }
        m_graveyard.clear();
        m_pendingRearm.clear();
        m_pendingArmRetry.clear();
        m_pendingSyntheticReady.clear();
    }

    Platform::EpollHandle Iocp::fileDescriptor() const noexcept
    {
        return m_iocp;
    }

    bool Iocp::addFileDescriptor(const int fileDescriptor, const std::uint32_t events, void *const userData)
    {
        if (m_sockets.contains(fileDescriptor))
        {
            // 调用方多半是把两个注册对象套在了同一个描述符上
            return false;
        }

        auto *state         = new SocketState();
        state->socketHandle = static_cast<SOCKET>(fileDescriptor);
        state->initProbes(userData);

        // 监听态决定读探针的形态：AcceptEx 还是 MSG_PEEK 的 WSARecv
        int  acceptConnection = 0;
        int  optionLength     = static_cast<int>(sizeof(acceptConnection));
        state->isListening =
                ::getsockopt(state->socketHandle, SOL_SOCKET, SO_ACCEPTCONN, reinterpret_cast<char *>(&acceptConnection), &optionLength) == 0 &&
                acceptConnection != 0;

        if (::CreateIoCompletionPort(toHandle(state->socketHandle), m_iocp, reinterpret_cast<ULONG_PTR>(state), 0) == nullptr)
        {
            // 描述符无效或已属于另一个完成端口
            delete state;
            return false;
        }

        state->registeredEvents = events;
        m_sockets.emplace(fileDescriptor, state);

        // 初始关注位立刻武装一次，与 IoWatcher 构造时把 EPOLLIN 交给内核同一语义。
        // 投递失败不当成注册失败：连接还没建立、监听描述符暂时拿不到接受套接字都会失败，
        // 上层真正等待时会经 modFileDescriptor() 再武装一次
        if ((events & EPOLLIN) != 0)
        {
            static_cast<void>(armProbe(*state, EPOLLIN));
        }
        if ((events & EPOLLOUT) != 0)
        {
            static_cast<void>(armProbe(*state, EPOLLOUT));
        }
        return true;
    }

    bool Iocp::modFileDescriptor(const int fileDescriptor, const std::uint32_t events, void *const userData)
    {
        const auto iterator = m_sockets.find(fileDescriptor);
        if (iterator == m_sockets.end())
        {
            return false;
        }

        SocketState &state   = *iterator->second;
        state.userData       = userData;
        state.registeredEvents = events;

        if ((events & EPOLLIN) != 0)
        {
            static_cast<void>(armProbe(state, EPOLLIN));
        }
        if ((events & EPOLLOUT) != 0)
        {
            static_cast<void>(armProbe(state, EPOLLOUT));
        }
        return true;
    }

    bool Iocp::delFileDescriptor(const int fileDescriptor)
    {
        const auto iterator = m_sockets.find(fileDescriptor);
        if (iterator == m_sockets.end())
        {
            return false;
        }

        SocketState &state = *iterator->second;
        m_sockets.erase(iterator);
        // 此刻可能已经有一条完成通知被取出来、排进了待重武装表，或者还在待重投表里：
        // 注销必须把它们一起摘掉，否则下一轮 wait() 会摸到已释放的状态
        std::erase(m_pendingRearm, &state);
        std::erase(m_pendingArmRetry, &state);
        // 待合成表也要一起摘：这条状态可能马上就在这里被 delete，留着指针下一轮 wait() 就是摸已释放内存
        std::erase(m_pendingSyntheticReady, &state);

        cancelProbes(state);
        if (state.hasProbeInFlight())
        {
            // 取消是异步的：完成通知仍会到达，状态必须活到那时再释放，
            // 否则 wait() 翻译那条通知时会写到已释放内存上
            state.isDeleted = true;
            m_graveyard.push_back(&state);
            return true;
        }

        closeAcceptedSockets(state);
        delete &state;
        return true;
    }

    bool Iocp::takeAcceptedSocket(const int listenerFileDescriptor, int *const acceptedFileDescriptor)
    {
        const auto iterator = m_sockets.find(listenerFileDescriptor);
        if (iterator == m_sockets.end())
        {
            return false;
        }

        SocketState &state = *iterator->second;
        if (!state.hasAcceptedSocket)
        {
            return false;
        }

        *acceptedFileDescriptor = static_cast<int>(state.acceptedSocket);
        state.acceptedSocket    = INVALID_SOCKET;
        state.hasAcceptedSocket = false;
        return true;
    }

    /**
     * @brief 探针投递失败是否属于「等下一拍再来」的可重试错误
     * @details 连接尚未建立（监听描述符还没 listen()、客户端套接字还没 connect 完）与发送缓冲
     *          暂时满都会自愈，值得下一拍重投；其余（对端复位、描述符已失效、参数非法）是硬错误，
     *          再重投也不会变好——那条路径交给 wait() 合成一条错误事件让等待方立刻收尾
     * @param socketError WSASend/WSARecv 返回的错误码
     * @return true 可重试
     */
    [[nodiscard]] static bool isRetryableProbeFailure(const int socketError) noexcept
    {
        return socketError == WSAEWOULDBLOCK || socketError == WSAENOTCONN || socketError == WSAEINPROGRESS;
    }

    bool Iocp::armProbe(SocketState &state, const std::uint32_t direction)
    {
        if (direction == EPOLLIN)
        {
            if (state.hasReadProbe)
            {
                return true;
            }
            // 监听态要在这里现查而不能只信注册时那一份：注册发生在第一次等待上
            // （AsyncSocket::ensureWatcher 是惰性的），若那次等待早于 bind()/listen()，登记到的就不是
            // 监听态，而按普通套接字投 WSARecv 会得到 10057 且此后没有任何完成通知（实测：接受路径
            // 整个卡死）。连接性一旦确认过就不再查：已连上的套接字不会变回监听态，而这次查询与下面的
            // getpeername 都是内核往返，落在「每条可读事件后的重新武装」这条热路径上
            if (!state.isListening && state.isConnectivitySettled != 1)
            {
                int acceptConnection = 0;
                int optionLength     = static_cast<int>(sizeof(acceptConnection));
                state.isListening = ::getsockopt(state.socketHandle, SOL_SOCKET, SO_ACCEPTCONN,
                                                 reinterpret_cast<char *>(&acceptConnection), &optionLength) == 0 &&
                                    acceptConnection != 0;
            }
            if (state.isListening)
            {
                // AcceptEx 投递失败（还没 listen()、拿不到接受套接字等）：armAcceptProbe 已自行记下待重投
                return armAcceptProbe(state);
            }

            // 套接字类型现查一次并记住：数据报套接字是下面那条「先确认已连上」守卫的例外
            if (state.isDatagramSocket < 0)
            {
                int socketType   = 0;
                int optionLength = static_cast<int>(sizeof(socketType));
                state.isDatagramSocket =
                        ::getsockopt(state.socketHandle, SOL_SOCKET, SO_TYPE, reinterpret_cast<char *>(&socketType), &optionLength) == 0 &&
                                        socketType == SOCK_DGRAM
                                ? 1
                                : 0;
                if (state.isDatagramSocket == 1)
                {
                    // 数据报无连接：连接性探测（getpeername）对它没有意义，此后一并跳过
                    state.isConnectivitySettled = 1;
                }
            }

            // 还没连上的套接字不能投读探针：客户端套接字在 connect 完成之前、监听描述符在 listen()
            // 之前，WSARecv 有时当场失败、有时挂起成一个永远不会完成的请求——后者会让监听描述符
            // 带着一个假探针，此后再也等不到 AcceptEx（实测：同一进程里第一条连接正常、之后的
            // 监听全都接不进连接）。连没连上用 getpeername 判，不通过就记成待重试。
            // 数据报套接字不受这条限制：UDP 无连接，getpeername 必然失败，而 WSARecv 在无连接 UDP 上
            // 是合法的——它会在任意一条报文到达时完成，正是要的就绪信号（少了这条豁免，数据报的
            // 可读等待在 Windows 上永远不会被唤醒）
            if (state.isDatagramSocket == 0)
            {
                sockaddr_storage peerAddress{};
                int              peerAddressLength = static_cast<int>(sizeof(peerAddress));
                if (::getpeername(state.socketHandle, reinterpret_cast<sockaddr *>(&peerAddress), &peerAddressLength) != 0)
                {
                    // 还没连上是每个客户端套接字注册时的常态（AsyncSocket 构造即注册 EPOLLIN，
                    // connect 还在后面），因此这一条不告警，只排进重投表等连上
                    noteArmPending(state, EPOLLIN);
                    return false;
                }
                // 连上了就不会再退回去：此后这次探测与上面的 SO_ACCEPTCONN 查询都不必再做
                state.isConnectivitySettled = 1;
            }

            std::memset(&state.readProbe.overlapped, 0, sizeof(OVERLAPPED));
            WSABUF readBuffer{};
            readBuffer.buf = state.readBuffer;
            readBuffer.len = sizeof(state.readBuffer);
            DWORD receiveFlags = MSG_PEEK;
            const int result   = ::WSARecv(state.socketHandle, &readBuffer, 1, nullptr, &receiveFlags, &state.readProbe.overlapped, nullptr);
            if (result == 0 || (result == SOCKET_ERROR && ::WSAGetLastError() == WSA_IO_PENDING))
            {
                state.hasReadProbe = true;
                state.failedDirections &= ~EPOLLIN;
                return true;
            }
            const int readSocketError = ::WSAGetLastError();
            if (!isRetryableProbeFailure(readSocketError))
            {
                // 硬错误（对端复位、描述符已失效）：这条套接字上不会再有任何完成通知，
                // 只记重投的话等待方永远收不到事件——合成一条错误事件让它立刻收尾，
                // 真实错误码由等待方自己的 recv/send 去拿，这里不再重复报一遍
                noteSyntheticReady(state, EPOLLIN);
            }
            else
            {
                // 可重试：这一方向此刻没有探针在途，之后也不会有完成通知，必须由后端自己再投一次
                // （上层看到的是「武装成功」，不会再要求武装）。静默处理：它属于暂时状态那一类，
                // 而持续失败的告警已由接受探针那条路径负责
                noteArmPending(state, EPOLLIN);
            }
            return false;
        }

        if (state.hasWriteProbe)
        {
            return true;
        }

        // 零字节发送：套接字可写时立刻完成，发送缓冲占满时挂到可写为止。
        // 它不向连接里写任何字节，因此不会污染字节流
        std::memset(&state.writeProbe.overlapped, 0, sizeof(OVERLAPPED));
        WSABUF emptyBuffer{};
        const int result = ::WSASend(state.socketHandle, &emptyBuffer, 1, nullptr, 0, &state.writeProbe.overlapped, nullptr);
        if (result == 0 || (result == SOCKET_ERROR && ::WSAGetLastError() == WSA_IO_PENDING))
        {
            state.hasWriteProbe = true;
            state.failedDirections &= ~EPOLLOUT;
            return true;
        }
        const int writeSocketError = ::WSAGetLastError();
        if (!isRetryableProbeFailure(writeSocketError))
        {
            // 与读侧同一处置：硬错误（对端复位后零字节 WSASend 直接返回 WSAECONNRESET，实测确认）
            // 不会再有任何完成通知，合成一条错误事件让等待方立刻去拿真实错误
            noteSyntheticReady(state, EPOLLOUT);
        }
        else
        {
            // 与读侧同一处置：可重试错误没有探针在途，必须排进重投表，否则这一方向永远静默
            noteArmPending(state, EPOLLOUT);
        }
        return false;
    }

    bool Iocp::armAcceptProbe(SocketState &state)
    {
        // 已经有一条在途的 AcceptEx，或已经接入一条还没被取走：都不需要再投
        if (state.pendingAcceptSocket != INVALID_SOCKET || state.hasAcceptedSocket)
        {
            state.failedDirections &= ~EPOLLIN;
            return true;
        }

        const LPFN_ACCEPTEX acceptFunction = resolveAcceptExFunction(state.socketHandle);
        if (acceptFunction == nullptr)
        {
            noteArmFailure(state, EPOLLIN, "本机不支持 AcceptEx（按 WSAID_ACCEPTEX 取函数指针失败）", ::WSAGetLastError());
            return false;
        }

        // 接受套接字必须与监听套接字同地址族，且带 WSA_FLAG_OVERLAPPED
        sockaddr_storage listenerAddress{};
        int              addressLength = static_cast<int>(sizeof(listenerAddress));
        if (::getsockname(state.socketHandle, reinterpret_cast<sockaddr *>(&listenerAddress), &addressLength) != 0)
        {
            noteArmFailure(state, EPOLLIN, "取监听地址失败（getsockname）", ::WSAGetLastError());
            return false;
        }

        // 用宽字符版（WSASocketW）：窄字符版在 /W4 下按已弃用 API 报 C4996，而这里本就没有字符串参数。
        // WSA_FLAG_NO_HANDLE_INHERIT 让这条预建的接受套接字不随 spawn 传下去，且不额外付一次系统调用
        const SOCKET acceptSocket = ::WSASocketW(
                static_cast<int>(listenerAddress.ss_family), SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
        if (acceptSocket == INVALID_SOCKET)
        {
            // 句柄或非分页内存耗尽时就是这里（WSAEMFILE / ENOBUFS）：监听描述符还在、
            // backlog 还在收，但一条也接不上来——不留这一行就只能看到「在监听却不应答」
            noteArmFailure(state, EPOLLIN, "创建接受套接字失败（WSASocket）", ::WSAGetLastError());
            return false;
        }

        // 地址缓冲只在监听套接字上分配一次：AcceptEx 每次都要它，而普通连接永远用不到
        if (state.acceptAddressBuffer == nullptr)
        {
            state.acceptAddressBuffer = std::make_unique<char[]>(kAcceptAddressBufferByteCount);
        }

        std::memset(&state.readProbe.overlapped, 0, sizeof(OVERLAPPED));
        DWORD      receivedByteCount = 0;
        const auto addressUnitByteCount = static_cast<DWORD>(kAcceptAddressBufferByteCount / 2);
        const BOOL isAccepted =
                acceptFunction(state.socketHandle, acceptSocket, state.acceptAddressBuffer.get(), 0, addressUnitByteCount, addressUnitByteCount,
                               &receivedByteCount, &state.readProbe.overlapped);
        if (isAccepted == FALSE && ::WSAGetLastError() != ERROR_IO_PENDING)
        {
            const int socketError = ::WSAGetLastError();
            ::closesocket(acceptSocket);
            noteArmFailure(state, EPOLLIN, "投递 AcceptEx 探针失败", socketError);
            return false;
        }

        state.pendingAcceptSocket = acceptSocket;
        state.hasReadProbe        = true;
        state.failedDirections &= ~EPOLLIN;
        return true;
    }

    void Iocp::noteArmPending(SocketState &state, const std::uint32_t direction)
    {
        state.failedDirections |= direction;
        if (!state.isArmRetryQueued)
        {
            state.isArmRetryQueued = true;
            m_pendingArmRetry.push_back(&state);
        }
    }

    void Iocp::noteArmFailure(SocketState &state, const std::uint32_t direction, const std::string_view reason, const int socketError)
    {
        // 告警要在置位之前判：失败位还清着才是「刚转为失败」，此后每轮 wait() 的重投都算重复
        const bool isFirstFailure = (state.failedDirections & direction) == 0;
        noteArmPending(state, direction);
        if (isFirstFailure)
        {
            LOG_WARN_FMT("IOCP 探针武装失败：{}（套接字 {}，错误码 {}）。该方向已排进重投表，每轮 wait() 前重试一次；"
                         "若持续失败，等它的协程不会收到任何事件",
                         reason, static_cast<std::uintptr_t>(state.socketHandle), socketError);
        }
    }

    void Iocp::retryFailedArms()
    {
        // 只处理进入时已在表里的那几条：armProbe 失败会由 noteArmPending 追加到**同一张表**的尾部，
        // 按长度取本轮份额、再把处理过的前几条摘掉，就不会边遍历边插入（此前是「整张表换到局部
        // 变量再销毁」，结果等价，但每轮都把缓冲还给堆、下一轮重新长出来）。
        // 表项是堆上的 SocketState 指针，扩容只搬表本身，按下标取用不受影响
        const std::size_t currentRoundCount = m_pendingArmRetry.size();

        // 逐条重试上一次投递失败的方向：注册成功但当时武装不上（监听描述符还没 listen()、
        // 套接字还没连上）是常态，失败必须在下一轮补上，否则那些描述符永远不会有完成通知
        for (std::size_t index = 0; index < currentRoundCount; ++index)
        {
            SocketState *state = m_pendingArmRetry[index];
            state->isArmRetryQueued = false;
            const std::uint32_t failedDirections = state->failedDirections;
            if ((failedDirections & EPOLLIN) != 0 && (state->registeredEvents & EPOLLIN) != 0)
            {
                static_cast<void>(armProbe(*state, EPOLLIN));
            }
            if ((failedDirections & EPOLLOUT) != 0 && (state->registeredEvents & EPOLLOUT) != 0)
            {
                static_cast<void>(armProbe(*state, EPOLLOUT));
            }
        }
        // 这里**不能**清表：本轮重投又失败的方向已经由 noteArmPending／noteArmFailure 重新入表，
        // 清掉它们等于「只重投一次」，之后那个描述符再也不会被武装——实测后果是监听描述符
        // 若在首次重投时还没 listen()，此后就永远等不到 AcceptEx，服务器不再接受任何连接。
        // 摘掉前 currentRoundCount 条即可：新排进来的被挪到表头，留给下一轮
        m_pendingArmRetry.erase(m_pendingArmRetry.begin(), m_pendingArmRetry.begin() + static_cast<std::ptrdiff_t>(currentRoundCount));
    }

    void Iocp::noteSyntheticReady(SocketState &state, const std::uint32_t direction)
    {
        state.readyDirections |= direction;
        // 一个状态在同一批里只占一个表项：两个方向都撞硬错误时由同一次合成一并带出
        if (!state.isSyntheticReadyQueued)
        {
            state.isSyntheticReadyQueued = true;
            m_pendingSyntheticReady.push_back(&state);
        }
    }

    void Iocp::harvestSyntheticErrorEvents()
    {
        // 绝大多数轮次没有任何硬错误：先按表空判定返回，稳态连一次取还堆块都不付
        if (m_pendingSyntheticReady.empty())
        {
            return;
        }

        // 直接遍历成员表：本函数体内不武装探针（只把已记下的错误位合成事件），因此本轮不会有
        // 新的入表操作，不需要像 retryFailedArms() 那样按本轮份额摘表。换表写法则每有一批硬错误
        // 就多付一次「还给堆、下轮重新长出来」的分配
        // 表里只有真正撞上硬错误的状态，因此这里的代价与本批条数成正比：每轮 wait() 都要走这一步，
        // 按注册表整体遍历会让「没有任何硬错误」的常态轮次也付出随连接数线性放大的成本
        for (SocketState *state: m_pendingSyntheticReady)
        {
            state->isSyntheticReadyQueued = false;
            const std::uint32_t directions = state->readyDirections;
            state->readyDirections         = 0;
            if (state->isDeleted)
            {
                // 已注销：通知只用来清账，上层的注册对象可能已经析构（与 translateCompletion 同一口径）
                continue;
            }
            epoll_event event{};
            event.data.ptr = state->userData;
            event.events   = directions | EPOLLERR | EPOLLHUP;
            const std::size_t existingResultIndex = findResultSlot(event.data.ptr);
            if (existingResultIndex != kEmptyResultSlot)
            {
                m_results[existingResultIndex].events |= event.events;
                continue;
            }
            noteResultSlot(event.data.ptr, m_results.size());
            m_results.push_back(event);
        }
        m_pendingSyntheticReady.clear();
    }

    std::span<epoll_event> Iocp::wait(const int timeoutMs)
    {
        // 补投上一次失败的探针：注册成功但当时武装不上（监听描述符还没 listen() 等）的方向
        // 必须在阻塞之前补上，否则这一睡就再也没有完成通知能把循环叫醒
        retryFailedArms();

        // 水平触发的关注由这里重新武装：上一次上报的数据此刻已经被上层消费掉，
        // 与 epoll_wait 每次重新取一次就绪状态等价（若先武装再交付，会被自己刚看到的数据
        // 立刻再触发一次，白白多绕一圈）
        rearmLevelTriggered();

        // 探针投递时就撞上硬错误的方向没有完成通知可等（对端复位后零字节 WSASend 直接返回
        // WSAECONNRESET，实测确认）：在这里合成错误事件，让等待方立刻醒来去拿真实错误。
        m_results.clear();
        resetResultMergeTable();
        harvestSyntheticErrorEvents();
        if (!m_results.empty())
        {
            return {m_results.data(), m_results.size()};
        }

        const ULONG timeout = timeoutMs < 0 ? INFINITE : static_cast<ULONG>(timeoutMs);
        DWORD       entryCount = 0;
        if (::GetQueuedCompletionStatusEx(m_iocp, m_entries.data(), static_cast<ULONG>(m_entries.size()), &entryCount, timeout, FALSE) == FALSE)
        {
            const DWORD errorCode = ::GetLastError();
            if (errorCode == WAIT_TIMEOUT || errorCode == WAIT_IO_COMPLETION)
            {
                return {};
            }
            throw Base::SystemException("等待完成端口失败（GetQueuedCompletionStatusEx）",
                                        std::error_code(static_cast<int>(errorCode), std::system_category()));
        }

        m_results.clear();
        resetResultMergeTable();
        for (DWORD index = 0; index < entryCount; ++index)
        {
            translateCompletion(m_entries[index]);
        }
        return {m_results.data(), m_results.size()};
    }

    void Iocp::translateCompletion(const OVERLAPPED_ENTRY &entry)
    {
        if (entry.lpOverlapped == nullptr)
        {
            // 本后端不投 PostQueuedCompletionStatus，出现这种条目说明完成端口被别的代码共用
            return;
        }

        auto         *context   = reinterpret_cast<ProbeContext *>(entry.lpOverlapped);
        SocketState  &state     = *context->owner;
        const std::uint32_t direction = context->direction;
        // 失败的完成（对端复位、取消、AcceptEx 出错）在 OVERLAPPED::Internal 上带负的状态码
        const bool isFailed = static_cast<LONG_PTR>(context->overlapped.Internal) < 0;

        if (direction == EPOLLIN)
        {
            state.hasReadProbe = false;
        } else
        {
            state.hasWriteProbe = false;
        }

        if (state.isListening && direction == EPOLLIN && state.pendingAcceptSocket != INVALID_SOCKET)
        {
            if (isFailed)
            {
                // AcceptEx 以失败完成（对端没等接入就断开、描述符已失效）：接受套接字没有用了，
                // 必须关掉并清空——留着它，下一次武装会被 armAcceptProbe 开头的
                // 「已有一条在途的 AcceptEx」挡住，该监听器此后再也不投递接受操作
                ::closesocket(state.pendingAcceptSocket);
                state.pendingAcceptSocket = INVALID_SOCKET;
            } else
            {
                // AcceptEx 成功：把接受套接字与监听套接字关联起来，此后它就是一条正常的已连接套接字
                const SOCKET acceptSocket = state.pendingAcceptSocket;
                static_cast<void>(::setsockopt(acceptSocket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                                               reinterpret_cast<const char *>(&state.socketHandle), sizeof(state.socketHandle)));
                state.pendingAcceptSocket = INVALID_SOCKET;
                state.acceptedSocket      = acceptSocket;
                state.hasAcceptedSocket   = true;
            }
        }

        if (state.isDeleted)
        {
            // 注销时取消的在途探针：通知只用来清账，不再上报
            releaseIfDrained(state);
            return;
        }

        if ((state.registeredEvents & direction) == 0)
        {
            // 这一方向的关注位已经被摘掉（清零掩码时旧探针还在途，或等待者刚好退场）：通知只清账、
            // 不上报。epoll 在这一刻根本不会把这个描述符报回来，而完成端口做得到——清零不回收已投出
            // 的操作。交上去的那一份就绪会被上层当成「有新数据」缓存起来，下一次等待凭空醒一次
            // （写侧更糟：一份陈旧的可写会让人以为缓冲已经排空）。探针标记已在上面清掉，
            // 重新关注时会另投一份，水平触发的语义不丢
            return;
        }

        if ((state.registeredEvents & EPOLLONESHOT) == 0)
        {
            m_pendingRearm.push_back(&state);
        }

        epoll_event event{};
        event.data.ptr = state.userData;
        event.events   = direction;
        if (isFailed)
        {
            // 与 epoll 一致：错误与挂断同时算作可读与可写，让上层自己去拿真实错误
            event.events |= EPOLLERR | EPOLLHUP;
        }

        // 同一个注册对象的两个方向可能在同一批完成通知里各来一条：**必须合并成一条 epoll_event**，
        // 这正是 epoll 给事件的方式（一个 epoll_event 带多个事件位）。分成两条时，上层处理第一条
        // 就可能把该注册对象销毁（读侧收到 EOF 就关连接是常态），第二条随后写到已释放内存上——
        // ASan 实测：IoWatcher::handleEvents 里往 m_armedEvents 写入时 heap-use-after-free。
        // 合并走「用户数据 → 结果下标」的扁平索引表：事件上限 1024 时线性查重最坏是 O(n²)，
        // 这是每轮 wait() 的热路径
        const std::size_t existingResultIndex = findResultSlot(event.data.ptr);
        if (existingResultIndex != kEmptyResultSlot)
        {
            m_results[existingResultIndex].events |= event.events;
            return;
        }
        noteResultSlot(event.data.ptr, m_results.size());
        m_results.push_back(event);
    }

    std::size_t Iocp::findResultSlot(void *const userData) const noexcept
    {
        std::size_t slot = hashResultSlotKey(userData, m_resultSlotMask);
        for (;;)
        {
            const std::size_t resultIndex = m_resultSlotIndex[slot];
            if (resultIndex == kEmptyResultSlot)
            {
                // 空槽即「查不到」：删除只发生在整表重置时，探测链上不会有被删出来的空槽
                return kEmptyResultSlot;
            }
            if (m_resultSlotUserData[slot] == userData)
            {
                return resultIndex;
            }
            slot = (slot + 1) & m_resultSlotMask;
        }
    }

    void Iocp::noteResultSlot(void *const userData, const std::size_t resultIndex)
    {
        // 装载因子过半就翻倍：wait() 每批最多 1024 条，而排空路径（drainCompletions）会在一次
        // 调用里累积多批，表必须跟着长，否则探测会绕不出去
        if ((m_results.size() + 1) * 2 > m_resultSlotIndex.size())
        {
            growResultMergeTable();
        }
        insertResultSlot(userData, resultIndex);
    }

    void Iocp::insertResultSlot(void *const userData, const std::size_t resultIndex)
    {
        std::size_t slot = hashResultSlotKey(userData, m_resultSlotMask);
        while (m_resultSlotIndex[slot] != kEmptyResultSlot)
        {
            slot = (slot + 1) & m_resultSlotMask;
        }
        m_resultSlotUserData[slot] = userData;
        m_resultSlotIndex[slot]    = resultIndex;
        m_usedResultSlots.push_back(slot);
    }

    void Iocp::growResultMergeTable()
    {
        const std::size_t newSlotCount = m_resultSlotIndex.size() * 2;
        m_resultSlotUserData.assign(newSlotCount, nullptr);
        m_resultSlotIndex.assign(newSlotCount, kEmptyResultSlot);
        m_usedResultSlots.clear();
        m_usedResultSlots.reserve(newSlotCount);
        m_resultSlotMask = newSlotCount - 1;

        // 已收集的结果重新散一遍：扩容前后同一个键的位置会变，不重散就查不到了
        for (std::size_t resultIndex = 0; resultIndex < m_results.size(); ++resultIndex)
        {
            insertResultSlot(m_results[resultIndex].data.ptr, resultIndex);
        }
    }

    void Iocp::resetResultMergeTable() noexcept
    {
        // 只清本轮用过的槽位：空槽的判据是下标表里的哨兵值，键表留着上一轮的旧值不影响判定，
        // 而整表填充是 16 KB 级别的工作量，比清掉那几条完成通知还贵
        for (const std::size_t slot: m_usedResultSlots)
        {
            m_resultSlotIndex[slot] = kEmptyResultSlot;
        }
        m_usedResultSlots.clear();
    }

    void Iocp::cancelProbes(SocketState &state) noexcept
    {
        // CancelIoEx 只发起取消：探针随后仍会以 ERROR_OPERATION_ABORTED 完成并入队，
        // 由它的完成通知走正常回收路径（墓碑表 / 排空）。已经完成的探针返回
        // ERROR_NOT_FOUND——它的通知早已在路上或已取出，无需处理
        if (state.hasReadProbe)
        {
            static_cast<void>(::CancelIoEx(toHandle(state.socketHandle), &state.readProbe.overlapped));
        }
        if (state.hasWriteProbe)
        {
            static_cast<void>(::CancelIoEx(toHandle(state.socketHandle), &state.writeProbe.overlapped));
        }
    }

    void Iocp::drainCompletions() noexcept
    {
        // 排空阶段同样维护合并索引：translateCompletion 会往 m_results 里写，
        // 索引表不跟着清就与结果集对不上（析构路径只关心通知被取走，内容不再使用）
        m_results.clear();
        resetResultMergeTable();

        for (;;)
        {
            // 还有在途探针才值得继续收：一条完成通知清掉一个方向，收齐即退出
            bool hasProbeInFlight = false;
            for (const auto &[fileDescriptor, state]: m_sockets)
            {
                static_cast<void>(fileDescriptor);
                if (state->hasProbeInFlight())
                {
                    hasProbeInFlight = true;
                    break;
                }
            }
            if (!hasProbeInFlight)
            {
                for (const SocketState *state: m_graveyard)
                {
                    if (state->hasProbeInFlight())
                    {
                        hasProbeInFlight = true;
                        break;
                    }
                }
            }
            if (!hasProbeInFlight)
            {
                return;
            }

            DWORD entryCount = 0;
            // 取消本应立即完成；真收不齐（驱动异常）也不能让析构无限等下去
            if (::GetQueuedCompletionStatusEx(m_iocp, m_entries.data(), static_cast<ULONG>(m_entries.size()), &entryCount,
                                              kDrainTimeoutMilliseconds, FALSE) == FALSE)
            {
                return;
            }
            for (DWORD index = 0; index < entryCount; ++index)
            {
                translateCompletion(m_entries[index]);
            }
        }
    }

    void Iocp::closeAcceptedSockets(SocketState &state) noexcept
    {
        if (state.pendingAcceptSocket != INVALID_SOCKET)
        {
            ::closesocket(state.pendingAcceptSocket);
            state.pendingAcceptSocket = INVALID_SOCKET;
        }
        if (state.acceptedSocket != INVALID_SOCKET)
        {
            ::closesocket(state.acceptedSocket);
            state.acceptedSocket = INVALID_SOCKET;
        }
        state.hasAcceptedSocket = false;
    }

    void Iocp::releaseIfDrained(SocketState &state)
    {
        if (state.hasProbeInFlight())
        {
            return;
        }
        std::erase(m_graveyard, &state);
        closeAcceptedSockets(state);
        delete &state;
    }

    void Iocp::rearmLevelTriggered()
    {
        // 直接遍历成员表，**不像 retryFailedArms() 那样先换到局部变量**：这张表只在
        // translateCompletion 里入表，而它由 wait() 在本函数之后才调用，本轮内不会有人往里加，
        // 因此不存在迭代器失效。换表反而每轮都把缓冲还给堆、下一批入表再从零长起——
        // 水平触发下这张表每轮都非空，实测单是去掉这一处就把每轮 wait() 的分配从 12 次降到 1 次
        for (SocketState *state: m_pendingRearm)
        {
            if ((state->registeredEvents & EPOLLONESHOT) != 0)
            {
                // 期间已改成一次性关注：下一次关注由 modFileDescriptor() 武装
                continue;
            }
            if ((state->registeredEvents & EPOLLIN) != 0)
            {
                static_cast<void>(armProbe(*state, EPOLLIN));
            }
            if ((state->registeredEvents & EPOLLOUT) != 0)
            {
                static_cast<void>(armProbe(*state, EPOLLOUT));
            }
        }
        m_pendingRearm.clear();
    }
} // namespace AsynGyanis::Core
