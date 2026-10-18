#include "Core/Process/UpgradeChannel.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/System/PlatformError.h"
#include "Platform/System/ProcessInfo.h"

#include <charconv>
#include <thread>

namespace AsynGyanis::Core
{
    namespace
    {
        /**
         * @brief 把平台层最近一次失败折成一句可操作的中文原因
         * @param action 失败的是哪一步，进文案
         * @return std::string 文案带上错误码，运维查系统调用层时不必再猜
         */
        std::string describeLastFailure(const std::string_view action)
        {
            const int code = Platform::PlatformError::lastSocketErrorCode();
            return std::string{action} + "失败：底层错误码 " + std::to_string(code) + "（" + std::system_category().message(code) + "）";
        }

        /// 连接通道的重试间隔：交棒方建通道是毫秒级的事，间隔再短只是白烧 CPU
        constexpr std::chrono::milliseconds kConnectRetryInterval{50};

        /// systemd 的约定：交来的描述符从 3 开始连续排布（0/1/2 是标准输入输出与错误）
        constexpr int kFirstActivatedDescriptor = 3;
        /// 枚数的合理性上界：一个服务真配到几十条监听器已是罕见，超过这个数只会是坏掉的环境变量，
        /// 按「没有交来」处理而不是让进程在启动期分配一串没人认领的 fd
        constexpr std::uint64_t kMaximumActivatedListenerCount = 4096;
    } // namespace

    UpgradeHandoffExpected<UpgradeChannel> UpgradeChannel::open()
    {
        // 通道本体是套接字：Windows 上任何 socket 调用之前都要有 Winsock 初始化引用。
        // 这条引用交出去之后就没人管了，所以按不加配对的 initialize() 申请：一次换代
        // 只开一条通道，进程活多久它活多久，让配对的 finalize() 有机会把还在用的 Winsock
        // 拆掉才是更糟的选择
        static_cast<void>(Platform::Socket::initialize());
        auto endpoint = Platform::Socket::openHandoffChannel();
        if (endpoint.has_value())
        {
            return UpgradeChannel{std::move(*endpoint)};
        }
        // 平台层用 error_code 交回原因，这里翻成中文：报错要让部署的人看得懂往哪儿查
        const std::error_code failure = endpoint.error();
        return std::unexpected(UpgradeHandoffError{UpgradeHandoffErrorKind::SystemCallFailed,
                                                   "交接通道开不出来：底层错误码 " + std::to_string(failure.value()) + "（" + failure.message() + "）。" +
                                                           "POSIX 上先确认临时目录可写、且路径长度没超出 unix 套接字上限；Windows 上确认回环地址可用"});
    }

    UpgradeChannel::UpgradeChannel(Platform::Socket::HandoffChannelEndpoint endpoint) noexcept : m_endpoint(std::move(endpoint))
    {
    }

    UpgradeChannel::UpgradeChannel(UpgradeChannel &&other) noexcept : m_endpoint(std::move(other.m_endpoint))
    {
        // 移走之后源对象不能再有任何收尾动作：留着路径会让它析构时把新主人的套接字文件删掉
        other.m_endpoint.listener = -1;
        other.m_endpoint.address.clear();
        other.m_endpoint.socketFilePath.clear();
    }

    UpgradeChannel &UpgradeChannel::operator=(UpgradeChannel &&other) noexcept
    {
        if (this != &other)
        {
            closeChannel();
            m_endpoint                = std::move(other.m_endpoint);
            other.m_endpoint.listener = -1;
            other.m_endpoint.address.clear();
            other.m_endpoint.socketFilePath.clear();
        }
        return *this;
    }

    UpgradeChannel::~UpgradeChannel() noexcept
    {
        closeChannel();
    }

    const std::string &UpgradeChannel::address() const noexcept
    {
        return m_endpoint.address;
    }

    void UpgradeChannel::closeChannel() noexcept
    {
        Platform::Socket::closeHandoffChannel(m_endpoint);
    }

    UpgradeHandoffExpected<int> UpgradeChannel::waitForPeer(const std::chrono::milliseconds budget) noexcept
    {
        if (m_endpoint.listener < 0)
        {
            return std::unexpected(UpgradeHandoffError{UpgradeHandoffErrorKind::ChannelUnavailable, "交接通道已经收口，等不到新一代连上来"});
        }

        // 地址要在收口之前取走：closeChannel() 会把通道文件删掉并清空这份文本，
        // 而超时那句必须报出「哪个地址没人连」——那是排查时唯一还指向这次交接的信息
        const std::string channelAddress = m_endpoint.address;

        // 给出正数预算时先问「有没有」，再走 accept：acceptHandoffPeer 只会无限等，而这条通道的
        // 对端是另一个进程，它可能起崩后再也不连。预算内没人连就把通道收掉——通道是一次性的，
        // 留着它只会有第二个进程挤进一次已经作废的交接
        if (budget > std::chrono::milliseconds::zero())
        {
            const auto isPeerPending = Platform::Socket::waitForAcceptReady(m_endpoint.listener, budget);
            if (!isPeerPending)
            {
                closeChannel();
                return std::unexpected(UpgradeHandoffError{UpgradeHandoffErrorKind::SystemCallFailed,
                                                           "等待新一代连上交接通道失败：" + Platform::PlatformError::message(isPeerPending.error().value())});
            }
            if (!*isPeerPending)
            {
                closeChannel();
                return std::unexpected(UpgradeHandoffError{UpgradeHandoffErrorKind::DeadlineExpired,
                                                           "新一代没在 " + std::to_string(budget.count()) + " 毫秒内连上交接通道（地址 " + channelAddress + "），这次交接作废"});
            }
        }

        const int peerDescriptor = Platform::Socket::acceptHandoffPeer(m_endpoint.listener);
        // 监听端到此为止：这条通道只交给一个新一代，留着它只会让第三个进程挤进一次已经谈定的交接
        closeChannel();
        if (peerDescriptor < 0)
        {
            return std::unexpected(UpgradeHandoffError{UpgradeHandoffErrorKind::SystemCallFailed, describeLastFailure("等待新一代连上交接通道")});
        }
        return peerDescriptor;
    }

    UpgradeHandoffStatus UpgradeChannel::handOffListener(const int peerDescriptor, const int listenDescriptor, const std::uint64_t targetProcessId) noexcept
    {
        if (peerDescriptor < 0 || listenDescriptor < 0)
        {
            return std::unexpected(
                    UpgradeHandoffError{UpgradeHandoffErrorKind::InvalidDescriptor,
                                        "交出监听套接字失败：通道或监听描述符无效（通道 " + std::to_string(peerDescriptor) + "，监听 " + std::to_string(listenDescriptor) + "）"});
        }
        if (!Platform::Socket::writeListeningSocketHandoff(peerDescriptor, listenDescriptor, targetProcessId))
        {
            return std::unexpected(UpgradeHandoffError{UpgradeHandoffErrorKind::SystemCallFailed, describeLastFailure("交出监听套接字")});
        }
        return {};
    }

    UpgradeHandoffExpected<int> adoptHandedOverListener(const std::string_view address, const std::chrono::milliseconds connectBudget)
    {
        // 同 open()：新一代这一侧也只调套接字，先补一次 Winsock 引用（POSIX 上是空操作）
        static_cast<void>(Platform::Socket::initialize());
        const auto deadline = std::chrono::steady_clock::now() + connectBudget;
        int        channel  = -1;
        for (;;)
        {
            // 每次重试都换新描述符：connect 失败之后的套接字不再保证可用，同一个上面重连不算重试
            channel = Platform::Socket::connectHandoffChannel(address);
            if (channel >= 0)
            {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return std::unexpected(UpgradeHandoffError{UpgradeHandoffErrorKind::DeadlineExpired, "连不上交接通道「" + std::string(address) + "」：预算 " +
                                                                                                             std::to_string(connectBudget.count()) + " 毫秒已用完（" +
                                                                                                             describeLastFailure("连接交接通道") + "）"});
            }
            std::this_thread::sleep_for(kConnectRetryInterval);
        }

        const int listenerDescriptor = Platform::Socket::readListeningSocketHandoff(channel);
        static_cast<void>(Platform::FileDescriptor::close(channel));
        if (listenerDescriptor < 0)
        {
            // EBADF 在 POSIX 上几乎都是「通道用错了类型」：内核只在 unix 域里随 SCM_RIGHTS 送描述符
            return std::unexpected(
                    UpgradeHandoffError{UpgradeHandoffErrorKind::SystemCallFailed, "收下移交来的监听套接字失败：" + describeLastFailure("读交接通道") +
                                                                                           "。若错误码是 EBADF，说明字节收齐了而里面没有描述符——通道必须是 AF_UNIX 流套接字"});
        }
        return listenerDescriptor;
    }

    ActivationListeners parseActivationEnvironment(const std::optional<std::string> &listenPid, const std::optional<std::string> &listenFds, const long ownProcessId)
    {
        ActivationListeners listeners;
        if (!listenFds.has_value() || !listenPid.has_value())
        {
            return listeners; // 没走 socket activation：两个值任缺其一就等价于「没有交来」
        }

        const auto parseUnsigned = [](const std::string &text, std::uint64_t &outcome)
        {
            std::uint64_t parsed = 0;
            const char   *begin  = text.data();
            const char   *end    = begin + text.size();
            const auto    result = std::from_chars(begin, end, parsed);
            if (result.ec != std::errc{} || result.ptr != end)
            {
                return false; // 非纯数字（含空串、带符号、尾部空格）一律按读不出处理
            }
            outcome = parsed;
            return true;
        };

        std::uint64_t pid           = 0;
        const bool    isPidReadable = parseUnsigned(*listenPid, pid);

        // 枚数先按原样读出来：即使归属不是本进程，调用方也要能说出「环境交来几枚、为什么一枚都不接」
        std::uint64_t announcedCount = 0;
        if (parseUnsigned(*listenFds, announcedCount) && announcedCount <= kMaximumActivatedListenerCount)
        {
            listeners.announcedCount = announcedCount;
        }

        listeners.isForThisProcess = isPidReadable && pid == static_cast<std::uint64_t>(ownProcessId);
        if (!listeners.isForThisProcess)
        {
            // LISTEN_PID 不是本进程（或根本读不出）：这批 fd 是替别人收的，一枚都不能碰
            listeners.descriptors.clear();
            return listeners;
        }

        listeners.descriptors.reserve(static_cast<std::size_t>(listeners.announcedCount));
        for (std::uint64_t offset = 0; offset < listeners.announcedCount; ++offset)
        {
            listeners.descriptors.push_back(kFirstActivatedDescriptor + static_cast<int>(offset));
        }
        return listeners;
    }

    ActivationListeners activationListenerDescriptors()
    {
        return parseActivationEnvironment(Platform::ProcessInfo::environmentVariable("LISTEN_PID"), Platform::ProcessInfo::environmentVariable("LISTEN_FDS"),
                                          Platform::ProcessInfo::currentProcessId());
    }
} // namespace AsynGyanis::Core
