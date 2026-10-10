// ServiceNotification 单元测试：状态文本的单行化、$NOTIFY_SOCKET 三种地址形状的取舍，
// 以及两条真把数据报发进对端套接字的端到端判据（文件系统路径与 Linux 抽象命名空间各一条）
#include "Platform/System/ProcessInfo.h"
#include "Platform/System/ServiceNotification.h"

#include "CommonTestSupport.h"
#include "Platform/Platform.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#if !ASYN_PLATFORM_WIN32
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace
{
    /// 被测的环境变量名：本组用例自己设、自己复原，不留给下一个用例去猜它在不在
    constexpr const char *kVariable = "NOTIFY_SOCKET";

    /// 设一个值；传 std::nullopt 表示把这个变量删掉
    void setNotifySocket(const std::optional<std::string> &value)
    {
#if ASYN_PLATFORM_WIN32
        static_cast<void>(::_putenv_s(kVariable, value.has_value() ? value->c_str() : ""));
#else
        if (value.has_value())
        {
            static_cast<void>(::setenv(kVariable, value->c_str(), 1));
        } else
        {
            static_cast<void>(::unsetenv(kVariable));
        }
#endif
    }

#if !ASYN_PLATFORM_WIN32
    /// 抽象命名空间的名字要按进程与序号唯一，否则两个并行进程抢同一个名字会互相收到对方的数据报
    std::string makeAbstractName()
    {
        static std::atomic<std::uint64_t> sequence{0};
        return std::format("asyn-notify-{}-{}", static_cast<std::uint64_t>(::getpid()), sequence.fetch_add(1));
    }

    /**
     * @brief 建一个「假的服务管理器」：按抽象命名空间的形状 bind 一个数据报套接字
     * @details 这里的地址构造是**独立写的一份**，不复用被测代码里的 buildDestination：两边共用
     *          同一段代码时，一起写错同一处判据是抓不到的。对端按「首个 NUL 属于名字、长度按字节算」
     *          自己走一遍，才有资格说发送方送对了那个地址。
     * @param name 不带前导 '@' 的名字
     * @return int 已 bind 的描述符；失败返回 -1
     */
    int bindAbstractReceiver(const std::string &name)
    {
        const int descriptor = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (descriptor < 0)
        {
            return -1;
        }

        sockaddr_un address{};
        address.sun_family  = AF_UNIX;
        address.sun_path[0] = '\0';
        std::memcpy(address.sun_path + 1, name.data(), name.size());

        const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
        if (::bind(descriptor, reinterpret_cast<const sockaddr *>(&address), length) != 0)
        {
            static_cast<void>(::close(descriptor));
            return -1;
        }
        return descriptor;
    }

    /**
     * @brief 同上，但走文件系统路径那一支（对端 bind 出的是磁盘上的套接字文件）
     * @param path 套接字文件路径（由本用例自己创建与删除）
     * @return int 已 bind 的描述符；失败返回 -1
     */
    int bindPathReceiver(const std::filesystem::path &path)
    {
        const int descriptor = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (descriptor < 0)
        {
            return -1;
        }

        const std::string text = path.string();
        sockaddr_un       address{};
        address.sun_family = AF_UNIX;
        if (text.size() >= sizeof(address.sun_path))
        {
            static_cast<void>(::close(descriptor));
            return -1;
        }
        std::memcpy(address.sun_path, text.data(), text.size());
        address.sun_path[text.size()] = '\0';

        // 同名残留（上一次崩溃留下的）会直接让 bind 失败，先 unlink 再 bind
        static_cast<void>(::unlink(text.c_str()));
        if (::bind(descriptor, reinterpret_cast<const sockaddr *>(&address), static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + text.size() + 1)) != 0)
        {
            static_cast<void>(::close(descriptor));
            return -1;
        }
        return descriptor;
    }

    /**
     * @brief 收一条数据报；超时或出错回空串
     * @details 必须带超时：发送方没送对时用例要能红，而不是把整条 CI 作业挂在一次永久阻塞的
     *          recv 上——那种形态看起来像「作业还在跑」，最难归因。
     * @param descriptor 对端描述符
     * @return std::string 收到的字节
     */
    std::string receiveOneDatagram(const int descriptor)
    {
        timeval timeout{2, 0};
        static_cast<void>(::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));

        std::array<char, 512> buffer{};
        const auto            received = ::recv(descriptor, buffer.data(), buffer.size(), 0);
        if (received <= 0)
        {
            return {};
        }
        return std::string(buffer.data(), static_cast<std::size_t>(received));
    }
#endif
} // namespace

namespace AsynGyanis::Platform
{
    /**
     * @brief 每个用例进出都把 $NOTIFY_SOCKET 恢复原样
     * @details 本组用例会改这个变量；不复原的话后跑的用例会读到上一个用例留下的地址，
     *          表现为「open() 突然成功」这种随执行顺序而变的形态。
     */
    class ServiceNotificationTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            m_savedValue = ProcessInfo::environmentVariable(kVariable);
        }

        void TearDown() override
        {
            setNotifySocket(m_savedValue);
        }

        /// 进入本用例时的原值；本来没有就是 nullopt，TearDown 据此把变量删掉
        std::optional<std::string> m_savedValue{};
    };

    TEST(ServiceNotification, StatusStateKeepsPrintableTextAndFoldsEveryControlCharacter)
    {
        // 正常文本原样交出，含中文：UTF-8 的高位字节不是控制字符，不该被动过
        EXPECT_EQ(ServiceNotification::statusState("正在重读配置"), "STATUS=正在重读配置");

        // 换行会把一条状态在对端切成两条赋值，因此折成空格；制表、CR、DEL 与 0x01 同判
        EXPECT_EQ(ServiceNotification::statusState(std::string_view{"一行\n两行"}), "STATUS=一行 两行");
        EXPECT_EQ(ServiceNotification::statusState(std::string_view{"制表\t结束\r"}), "STATUS=制表 结束 ");
        EXPECT_EQ(ServiceNotification::statusState(std::string_view{"\x01"
                                                                    "A"
                                                                    "\x7F"}),
                  "STATUS= A ");

        // 空文本仍交出一条合法赋值：STATUS= 后面没有内容，对端读成「没有补充说明」，
        // 而不是把整条赋值省掉——「RELOADING=1」与「RELOADING=1\nSTATUS=」是两回事
        EXPECT_EQ(ServiceNotification::statusState(std::string_view{}), "STATUS=");
    }

#if ASYN_PLATFORM_WIN32

    TEST_F(ServiceNotificationTest, WindowsRefusesEveryEntryWithThePlatformFact)
    {
        // Windows 没有这套约定：入口必须明确失败并给出同一句平台事实。
        // 静默成功的后果是部署方以为 READY=1 发出去了，而那一侧的 Type=notify 永远等不到
        ServiceNotification notification;
        EXPECT_FALSE(notification.open());
        EXPECT_FALSE(notification.isOpen());
        EXPECT_NE(notification.lastError().find("Windows"), std::string::npos) << notification.lastError();

        EXPECT_FALSE(notification.send(ServiceNotification::kReadyState));
        EXPECT_NE(notification.lastError().find("Windows"), std::string::npos) << notification.lastError();
    }

#else

    TEST_F(ServiceNotificationTest, MissingVariableSaysTheProcessIsNotSupervised)
    {
        setNotifySocket(std::nullopt);

        ServiceNotification notification;
        EXPECT_FALSE(notification.open());
        // 「没配置」必须与「配了但形状不认识」分得开：前者是按约定的正常形态，后者是部署错误
        EXPECT_NE(notification.lastError().find("NOTIFY_SOCKET"), std::string::npos) << notification.lastError();
        EXPECT_NE(notification.lastError().find("不在服务管理器的监督之下"), std::string::npos) << notification.lastError();
    }

    TEST_F(ServiceNotificationTest, EmptyVariableIsTreatedAsMissing)
    {
        setNotifySocket(std::string{});

        ServiceNotification notification;
        EXPECT_FALSE(notification.open());
        EXPECT_NE(notification.lastError().find("未配置"), std::string::npos) << notification.lastError();
    }

    TEST_F(ServiceNotificationTest, VsockAddressIsRefusedInsteadOfBeingTreatedAsAPath)
    {
        setNotifySocket(std::string{"vsock:2:1234"});

        ServiceNotification notification;
        EXPECT_FALSE(notification.open());
        // 这一句存在的理由：把 vsock: 当路径去连，报出来的是「没有那个文件或目录」，
        // 而实情是这条通路本层没接——两种原因的处置完全不同
        EXPECT_NE(notification.lastError().find("AF_VSOCK"), std::string::npos) << notification.lastError();
        EXPECT_FALSE(notification.isOpen());
    }

    TEST_F(ServiceNotificationTest, UnknownLeadingCharacterIsRefusedAndNamesTheAcceptedShapes)
    {
        setNotifySocket(std::string{"relative/path/notify.sock"});

        ServiceNotification notification;
        EXPECT_FALSE(notification.open());
        EXPECT_NE(notification.lastError().find("形状不认识"), std::string::npos) << notification.lastError();
    }

    TEST_F(ServiceNotificationTest, AbstractPrefixWithoutANameIsRefused)
    {
        setNotifySocket(std::string{"@"});

        ServiceNotification notification;
        EXPECT_FALSE(notification.open());
        EXPECT_NE(notification.lastError().find("名字不能是空的"), std::string::npos) << notification.lastError();
    }

    TEST_F(ServiceNotificationTest, OverlongFilesystemPathIsRefusedBeforeTheSocketIsBuilt)
    {
        // sockaddr_un::sun_path 是定长的；超上限的地址根本发不出去，与其把截断后的路径交给内核
        // （那会连到另一个名字不相干的套接字上）不如在建套接字之前就拒
        setNotifySocket(std::string{"/" + std::string(400, 'a')});

        ServiceNotification notification;
        EXPECT_FALSE(notification.open());
        EXPECT_NE(notification.lastError().find("过长"), std::string::npos) << notification.lastError();
    }

    TEST_F(ServiceNotificationTest, SendBeforeOpenIsRefusedAndPointsAtOpen)
    {
        setNotifySocket(std::string{"@asyn-notify-not-opened"});

        ServiceNotification notification;
        EXPECT_FALSE(notification.isOpen());
        EXPECT_FALSE(notification.send(ServiceNotification::kReadyState));
        EXPECT_NE(notification.lastError().find("open()"), std::string::npos) << notification.lastError();
    }

    TEST_F(ServiceNotificationTest, ReadyStateReachesAnAbstractNamespaceReceiver)
    {
        // 端到端判据：不是「open() 返回 true」，而是对端真收到了那一条字节。
        // 抽象命名空间是容器与 systemd 早期会话里常见的那一种，它的地址长度算法与文件系统
        // 那一支不同（首个 NUL 属于名字、结尾不补 NUL），错了会连到别的名字上而不是报错
        const std::string name     = makeAbstractName();
        const int         receiver = bindAbstractReceiver(name);
        ASSERT_GE(receiver, 0) << "对端 bind 抽象命名空间失败，本用例无从判定";

        setNotifySocket(std::string{"@"} + name);
        ServiceNotification notification;
        ASSERT_TRUE(notification.open()) << notification.lastError();
        ASSERT_TRUE(notification.send(ServiceNotification::kReadyState)) << notification.lastError();

        EXPECT_EQ(receiveOneDatagram(receiver), "READY=1");
        static_cast<void>(::close(receiver));
    }

    TEST_F(ServiceNotificationTest, ReadyThenStatusReachAFilesystemPathReceiverInOrder)
    {
        const std::filesystem::path directory  = std::filesystem::temp_directory_path();
        const std::filesystem::path socketPath = directory / std::format("asyn-notify-{}.sock", static_cast<std::uint64_t>(::getpid()));
        const int                   receiver   = bindPathReceiver(socketPath);
        ASSERT_GE(receiver, 0) << "对端 bind 套接字文件失败，本用例无从判定";

        setNotifySocket(socketPath.string());
        ServiceNotification notification;
        ASSERT_TRUE(notification.open()) << notification.lastError();

        // 两个数据报按发送顺序到达（同一对端的数据报不乱序）：监督者先看到「就绪」再看到补充说明，
        // 反过来的话 Type=notify 那侧会先等到一句没有意义的状态
        ASSERT_TRUE(notification.send(ServiceNotification::kReadyState)) << notification.lastError();
        ASSERT_TRUE(notification.send(ServiceNotification::statusState("正在重读配置"))) << notification.lastError();

        EXPECT_EQ(receiveOneDatagram(receiver), "READY=1");
        EXPECT_EQ(receiveOneDatagram(receiver), "STATUS=正在重读配置");

        static_cast<void>(::close(receiver));
        static_cast<void>(::unlink(socketPath.string().c_str()));
    }

    TEST_F(ServiceNotificationTest, StoppingStateIsSentAsItsOwnDatagram)
    {
        const std::string name     = makeAbstractName();
        const int         receiver = bindAbstractReceiver(name);
        ASSERT_GE(receiver, 0) << "对端 bind 抽象命名空间失败，本用例无从判定";

        setNotifySocket(std::string{"@"} + name);
        ServiceNotification notification;
        ASSERT_TRUE(notification.open()) << notification.lastError();
        ASSERT_TRUE(notification.send(ServiceNotification::kStoppingState)) << notification.lastError();

        // 停机那条单独成报：监督者据此开始计停机超时，把它并进别的状态里就等于晚一点才开始计
        EXPECT_EQ(receiveOneDatagram(receiver), "STOPPING=1");
        static_cast<void>(::close(receiver));
    }

    TEST_F(ServiceNotificationTest, EmptyStateIsRefusedWhileTheChannelIsOpen)
    {
        const std::string name     = makeAbstractName();
        const int         receiver = bindAbstractReceiver(name);
        ASSERT_GE(receiver, 0) << "对端 bind 抽象命名空间失败，本用例无从判定";

        setNotifySocket(std::string{"@"} + name);
        ServiceNotification notification;
        ASSERT_TRUE(notification.open()) << notification.lastError();

        // 空串要在进系统调用之前就被拒：发一个零长数据报，对端读到的是「一条空状态」，
        // 而那与「什么都没发」在监督者侧不是同一件事
        EXPECT_FALSE(notification.send(std::string_view{}));
        EXPECT_NE(notification.lastError().find("空的状况串"), std::string::npos) << notification.lastError();

        // 拒了之后通路仍然可用：一次用法错误不该把这条通知通路整体废掉
        EXPECT_TRUE(notification.send(ServiceNotification::kReadyState)) << notification.lastError();
        EXPECT_EQ(receiveOneDatagram(receiver), "READY=1");
        static_cast<void>(::close(receiver));
    }

    TEST_F(ServiceNotificationTest, SendFailureIsReportedWhenNothingIsListeningAtThatName)
    {
        // 有 NOTIFY_SOCKET 但对端没人收：数据报交不出去是 ECONNREFUSED，必须报出来。
        // 这一条盯的是「静默成功」那个方向——状态没送到而调用方以为送到了，
        // 后果是监督者按一个永远不会到来的 READY=1 判定启动失败
        setNotifySocket(std::string{"@"} + makeAbstractName());

        ServiceNotification notification;
        ASSERT_TRUE(notification.open()) << notification.lastError();
        EXPECT_FALSE(notification.send(ServiceNotification::kReadyState)) << "对端不存在却报成功";
        EXPECT_NE(notification.lastError().find("发送服务状态失败"), std::string::npos) << notification.lastError();
        EXPECT_NE(notification.lastError().find("错误码"), std::string::npos) << notification.lastError();
    }

    TEST_F(ServiceNotificationTest, CloseThenSendFailsAndOpeningAgainWorks)
    {
        const std::string name     = makeAbstractName();
        const int         receiver = bindAbstractReceiver(name);
        ASSERT_GE(receiver, 0) << "对端 bind 抽象命名空间失败，本用例无从判定";

        setNotifySocket(std::string{"@"} + name);
        ServiceNotification notification;
        ASSERT_TRUE(notification.open()) << notification.lastError();

        notification.close();
        EXPECT_FALSE(notification.isOpen());
        EXPECT_FALSE(notification.send(ServiceNotification::kReadyState));

        // 重新 open() 要能再次可用：调用方可能在一次收尾之后又起一轮（热升级那类形态）
        ASSERT_TRUE(notification.open()) << notification.lastError();
        EXPECT_TRUE(notification.send(ServiceNotification::kReadyState)) << notification.lastError();
        EXPECT_EQ(receiveOneDatagram(receiver), "READY=1");

        // 重复 close() 是空操作，不该报错，也不该把一个已经关掉的描述符再关一次
        notification.close();
        notification.close();
        static_cast<void>(::close(receiver));
    }

#endif
} // namespace AsynGyanis::Platform
