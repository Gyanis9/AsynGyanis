#include "Platform/System/ServiceNotification.h"
#include "Platform/Platform.h"
#include "Platform/System/PlatformError.h"
#include "Platform/System/ProcessInfo.h"

#include <cstddef>
#include <cstring>
#include <format>
#include <string>
#include <string_view>

#if !ASYN_PLATFORM_WIN32
// 数据报套接字与 sockaddr_un 的声明：本类的全部系统调用都在这两个头里
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace
{
    /// 环境变量名：systemd 用它把通知套接字交给服务进程
    constexpr const char *kNotifySocketVariable = "NOTIFY_SOCKET";

    /// 抽象命名空间前缀（Linux）：文档写的是首字符 '@'，其后整个串都是名字，不再当路径解释
    constexpr char kAbstractPrefix = '@';

    /// AF_VSOCK 前缀：文档认可的第三种形状，本层刻意不接
    constexpr std::string_view kVsockPrefix = "vsock:";

    /// 单行状态里替掉控制字符的字符
    constexpr char kControlReplacement = ' ';

    /// 平台事实那一句：Windows 分支的所有入口都回它，避免同一件事在四处写出四种说法
    constexpr const char *kUnsupportedPlatformText = "当前平台没有服务管理器的这条通知通路（NOTIFY_SOCKET 是 systemd 一侧的约定）："
                                                     "本引擎在 Windows 上不提供状态通知";
} // namespace

namespace AsynGyanis::Platform
{
    std::string ServiceNotification::statusState(std::string_view text)
    {
        std::string state = "STATUS=";
        state.reserve(state.size() + text.size());

        for (const char raw: text)
        {
            // 控制字符一律折成空格：文档要的是**单行**状态，留着换行就使「一条状态」在对端变成
            // 「两条赋值」，其中一条还是没人认识的键。
            // 判据按无符号位走，且高位字节当作可打印文本原样留下——中文的每一字节都 >= 0x80，
            // 它们既不是控制字符，也不该被拆坏；把 signed char 直接喂给按值的判定是未定义行为
            const auto unsignedValue = static_cast<unsigned char>(raw);
            const bool isControl     = (unsignedValue < 0x20U) || (unsignedValue == 0x7FU);
            state.push_back(isControl ? kControlReplacement : raw);
        }

        return state;
    }

    ServiceNotification::~ServiceNotification()
    {
        close();
    }

    bool ServiceNotification::open()
    {
        m_lastError.clear();

#if ASYN_PLATFORM_WIN32
        m_lastError = kUnsupportedPlatformText;
        return false;
#else
        const auto configuredAddress = ProcessInfo::environmentVariable(kNotifySocketVariable);
        if (!configuredAddress.has_value() || configuredAddress->empty())
        {
            // 这一句是「不是缺陷」的说明：没有这个变量就是不通知，而调用方需要能分辨
            // 「本进程没在监督下跑」与「监督在但套接字发不出去」，两者的处置完全不同
            m_lastError = "未配置 NOTIFY_SOCKET：本进程不在服务管理器的监督之下，没有状态可发（按 systemd 的约定这不是失败）";
            return false;
        }

        if (!buildDestination(*configuredAddress))
        {
            return false;
        }

        // SOCK_CLOEXEC：通知套接字不该跟着 exec 活到下一个程序里去（exec 之后发出去的数据报
        // 会算到那个程序头上，而它并不知道自己欠谁一条 READY=1）
        const int descriptor = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (descriptor < 0)
        {
            const int errorCode = PlatformError::lastErrorCode();
            m_lastError         = std::format("建 NOTIFY_SOCKET 的数据报套接字失败：{}（错误码 {}）", PlatformError::message(errorCode), errorCode);
            return false;
        }

        m_descriptor = descriptor;
        return true;
#endif
    }

    bool ServiceNotification::isOpen() const noexcept
    {
        return m_descriptor >= 0;
    }

    bool ServiceNotification::send(std::string_view state) noexcept
    {
        m_lastError.clear();

#if ASYN_PLATFORM_WIN32
        static_cast<void>(state);
        m_lastError = kUnsupportedPlatformText;
        return false;
#else
        if (m_descriptor < 0)
        {
            m_lastError = "还没打开通知通路就发了状态：先判 open() 的返回值，它已经把「没有 NOTIFY_SOCKET」与「形状不支持」分开说了";
            return false;
        }

        if (state.empty())
        {
            m_lastError = "空的状况串发不出去：一条也没有的赋值对端读成「什么都不知道」，不如就地报错";
            return false;
        }

        const auto destination = reinterpret_cast<const sockaddr *>(m_destination.data());
        const auto sent        = ::sendto(m_descriptor, state.data(), state.size(), 0, destination, static_cast<socklen_t>(m_destinationLength));
        if (sent < 0)
        {
            const int errorCode = PlatformError::lastErrorCode();
            m_lastError         = std::format("发送服务状态失败：{}（错误码 {}）", PlatformError::message(errorCode), errorCode);
            return false;
        }

        if (static_cast<std::size_t>(sent) != state.size())
        {
            // 数据报本不该交出「半条」；真撞上就是内核层的事，宁可报错也不让调用方以为整条状态到了
            m_lastError = std::format("服务状态只交出了 {} / {} 字节：数据报没有部分送达这回事，这条按没发成功处理", sent, state.size());
            return false;
        }

        return true;
#endif
    }

    void ServiceNotification::close() noexcept
    {
#if !ASYN_PLATFORM_WIN32
        if (m_descriptor >= 0)
        {
            static_cast<void>(::close(m_descriptor));
        }
#endif
        m_descriptor        = -1;
        m_destinationLength = 0;
        m_destination.clear();
    }

    const std::string &ServiceNotification::lastError() const noexcept
    {
        return m_lastError;
    }

#if !ASYN_PLATFORM_WIN32
    bool ServiceNotification::buildDestination(const std::string &addressText) noexcept
    {
        sockaddr_un destination{};
        destination.sun_family = AF_UNIX;

        // sun_path 是定长字符数组：文件系统那一支还要留一个结尾 NUL，抽象那一支不留（见下）
        const std::size_t pathCapacity = sizeof(destination.sun_path);
        const std::size_t pathOffset   = offsetof(sockaddr_un, sun_path);

        if (addressText.starts_with(kVsockPrefix))
        {
            m_lastError = "NOTIFY_SOCKET 指的是 AF_VSOCK 地址（vsock: 前缀）：本层只接文件系统与抽象命名空间这两种，"
                          "不会把它当成一个文件名去连——那样报出来的是「没有那个文件或目录」，而真正的原因是这条通路没接";
            return false;
        }

        if (addressText.front() == kAbstractPrefix)
        {
            const std::string_view body = std::string_view(addressText).substr(1);
            if (body.empty())
            {
                m_lastError = "NOTIFY_SOCKET 只有一个 '@'：抽象命名空间的名字不能是空的，空名字等于未绑定，发过去没人收";
                return false;
            }
            if (body.size() > pathCapacity - 1)
            {
                m_lastError = std::format("NOTIFY_SOCKET 的抽象命名空间名字过长（{} 字节，上限 {}）：本层的地址缓冲装不下它", body.size(), pathCapacity - 1);
                return false;
            }

            // 抽象地址的第一个字节是 NUL 而不是串终止符，其后的字节全部属于名字本身：
            // 长度因此不带结尾 NUL（多算一个字节会连到另一个名字不相干的套接字上，见 sd(7) 对
            // 抽象地址「按长度取字节」的说明）
            destination.sun_path[0] = '\0';
            std::memcpy(destination.sun_path + 1, body.data(), body.size());
            m_destinationLength = pathOffset + 1 + body.size();
        } else
        {
            if (addressText.front() != '/')
            {
                m_lastError = std::format("NOTIFY_SOCKET 的形状不认识：首字符是 '{}'，而文档只给了 '/'（文件系统）、'@'（抽象命名空间）与 \"vsock:\" 三种", addressText.front());
                return false;
            }
            if (addressText.size() > pathCapacity - 1)
            {
                m_lastError = std::format("NOTIFY_SOCKET 的套接字路径过长（{} 字节，上限 {}）：本层的 sockaddr_un 缓冲装不下它", addressText.size(), pathCapacity - 1);
                return false;
            }

            std::memcpy(destination.sun_path, addressText.data(), addressText.size());
            destination.sun_path[addressText.size()] = '\0';
            m_destinationLength                      = pathOffset + addressText.size() + 1;
        }

        const auto *const rawBytes = reinterpret_cast<const char *>(&destination);
        m_destination.assign(rawBytes, rawBytes + m_destinationLength);
        return true;
    }
#endif
} // namespace AsynGyanis::Platform
