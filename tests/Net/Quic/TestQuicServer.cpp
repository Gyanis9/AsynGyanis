// QUIC 服务端的构造期与配置校验用例：这两类判据不需要对端，因此留在树内。
//
// 对外行为（握手、ALPN、流数据与流收尾、drain 时的 CONNECTION_CLOSE、半开连接的回收、
// 跨实例会话恢复、单来源限额）**不再由链接进来的第三方 QUIC 实现裁定**：那类「同一棵树里互测」
// 暴露不出双方共享的误解，还会跟着被测实现一起被改松。它们整体搬到进程外的独立实现上：
//     scripts/quic_cross_check.sh <构建目录> <带 aioquic 的 python>
// 被测端是 tests/Tools/QuicProbeServer.cpp，判据由 aioquic 的解析器给出。
// 改 QuicServer 的对外行为时，跑那条脚本才算过门禁；在本文件里加「自家客户端连自家服务端」
// 的用例等于把刚拆掉的裁判请回来。
#include "Net/Quic/QuicServer.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/SystemException.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Tcp/PerIpConnectionLimiter.h"

#include "CoreTestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 夹具等待上限
        constexpr std::chrono::milliseconds kWaitTimeout{8000};

        /// 服务器证书与私钥（与 TLS 用例共用同一份夹具）
        std::string certificatePath()
        {
            return std::string(TEST_FIXTURES_DIR) + "/test_cert.pem";
        }

        std::string privateKeyPath()
        {
            return std::string(TEST_FIXTURES_DIR) + "/test_key.pem";
        }

        /**
         * @brief 起一台回环上的 QUIC 服务端，把循环放到自己的线程上跑
         * @details 只为本文件的两条构造期判据服务：构造要落到一个真实的事件循环上，
         *          而端口与连接数这类成员只由循环线程读写，读它们一律走投递（跨线程直读
         *          就是与循环抢同一个成员，TSan 报过这一类）。
         */
        class RunningQuicServer
        {
        public:
            /**
             * @brief 起服务端
             * @param idleTimeout 空闲/握手超时
             * @param perIpConnectionLimiter 单来源并发上限的限额器；空表示不按来源限制
             * @param sessionTicketKeyFiles 会话票据密钥文件列表；空表示按 OpenSSL 默认
             */
            explicit RunningQuicServer(const std::chrono::seconds idleTimeout = std::chrono::seconds{30},
                                       std::shared_ptr<PerIpConnectionLimiter> perIpConnectionLimiter = nullptr,
                                       std::vector<std::string> sessionTicketKeyFiles = {})
            {
                QuicServer::Configuration configuration;
                configuration.certificateFile       = certificatePath();
                configuration.privateKeyFile        = privateKeyPath();
                configuration.idleTimeout           = idleTimeout;
                configuration.perIpConnectionLimiter = std::move(perIpConnectionLimiter);
                configuration.sessionTicketKeyFiles  = std::move(sessionTicketKeyFiles);

                m_server = std::make_unique<QuicServer>(m_loop, configuration);
                m_listenTask.emplace(m_server->listen(Core::InetAddress::resolve("127.0.0.1", 0).value()));
                // 在起线程之前把自己排进所属循环的就绪队列：调度器只在循环线程上跑，
                // 这里还在创建者线程上，于是这一次排入是「归属线程内」的合法调用
                m_loop.scheduler().schedule(m_listenTask->handle());

                m_loopThread = std::thread([this]
                {
                    m_loop.run();
                });

                const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
                while (refreshListeningPort() == 0 && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                }
            }

            ~RunningQuicServer()
            {
                m_server->stop();
                m_loop.stop();
                if (m_loopThread.joinable())
                {
                    m_loopThread.join();
                }
            }

            RunningQuicServer(const RunningQuicServer &) = delete;

            RunningQuicServer &operator=(const RunningQuicServer &) = delete;

        private:
            /**
             * @brief 在循环线程上执行一段动作，并等它做完
             * @param action 待执行的动作
             */
            void runOnLoopAndWait(const std::function<void()> &action)
            {
                std::atomic<bool> isFinished{false};
                m_loop.scheduler().postRemote(
                        [&action, &isFinished]
                        {
                            action();
                            isFinished.store(true, std::memory_order_release);
                        });

                const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
                while (!isFinished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                EXPECT_TRUE(isFinished.load(std::memory_order_acquire)) << "投递到循环线程的动作没有在时限内完成";
            }

            /// 在循环线程上取一次本端端口，更新快照后返回
            std::uint16_t refreshListeningPort()
            {
                std::uint16_t port = 0;
                runOnLoopAndWait([this, &port] { port = m_server->listeningPort(); });
                m_listeningPort.store(port, std::memory_order_release);
                return port;
            }

            Core::EventLoop                 m_loop;             ///< 服务端所属事件循环
            std::unique_ptr<QuicServer>     m_server;           ///< 被测服务端
            std::optional<Core::Task<>>     m_listenTask;       ///< 监听协程（Task 没有默认构造，用 optional 托管）
            std::thread                     m_loopThread;       ///< 跑循环的线程
            std::atomic<std::uint16_t>      m_listeningPort{0}; ///< 循环线程写下的本端端口快照
        };
    } // namespace

    /**
     * @brief 票据密钥不合格时构造当场失败，而不是悄悄退回「每个上下文一份随机密钥」
     * @details 退回随机密钥的表现是恢复命中率归零而服务一切正常——既没有错误日志也没有失败请求，
     *          是这类配置最贵的失败形态。异常文本还要点名是哪一份文件：本条失败没有 OpenSSL
     *          错误栈可查
     */
    TEST(QuicServer, RejectsInvalidTicketKeyFilesDuringConstruction)
    {
        AsynGyanis::TestSupport::TemporaryDirectory directory("QuicTicketKeyInvalid");
        const std::string                           wrongLengthPath = (directory.path() / "wrong-length.key").string();
        EXPECT_TRUE(directory.writeBinaryFile("wrong-length.key", AsynGyanis::TestSupport::makeBytePattern(31, 64)));

        const std::vector<std::string> keyFiles{wrongLengthPath};
        try
        {
            static_cast<void>(std::make_unique<RunningQuicServer>(std::chrono::seconds{30}, nullptr, keyFiles));
            FAIL() << "长度为 64 字节的密钥文件（既不是 48 也不是 80）应当让构造失败";
        } catch (const Base::Exception &failure)
        {
            const std::string message = failure.what();
            EXPECT_TRUE(message.find("wrong-length.key") != std::string::npos) << "异常没点名是哪一份文件：" << message;
        }

        // 文件根本不存在同样要抛，且不能留下半构造的对象：上面那台已经抛在构造期，
        // 这里的断言只判「会不会抛」，端口与握手都不参与
        EXPECT_THROW(static_cast<void>(std::make_unique<RunningQuicServer>(
                             std::chrono::seconds{30}, nullptr, std::vector<std::string>{(directory.path() / "missing.key").string()})),
                     Base::Exception);
    }

    /**
     * @brief 构造期证书或私钥不合规时当场抛出，且不留下一份无人认领的 TLS 上下文
     * @details SSL_CTX 是构造里第一件拿到的资源，其后四道检查任一不过都抛——那时析构函数不会跑，
     *          旧写法存在成员里的裸指针就此没人负责（实测一份 1784 字节直漏 + 连带 35 KiB 的间接量，
     *          每次构造失败漏一整份）。判据分两层：抛与不抛由这里钉，漏与不漏由容器的 LSan 门禁钉，
     *          两层缺一都挡不住「改成不抛而是吞掉」这种倒退。
     */
    TEST(QuicServer, ReleasesTlsContextWhenCertificateValidationFailsDuringConstruction)
    {
        // 构造与销毁都在本线程：这个循环没交给别的线程，本线程就是它的归属线程
        Core::EventLoop loop;

        QuicServer::Configuration missingCertificate;
        missingCertificate.certificateFile = std::string(TEST_FIXTURES_DIR) + "/no-such-cert.pem";
        missingCertificate.privateKeyFile  = privateKeyPath();
        EXPECT_THROW(QuicServer server(loop, missingCertificate), Base::SystemException);

        QuicServer::Configuration missingPrivateKey;
        missingPrivateKey.certificateFile = certificatePath();
        missingPrivateKey.privateKeyFile  = std::string(TEST_FIXTURES_DIR) + "/no-such-key.pem";
        EXPECT_THROW(QuicServer server(loop, missingPrivateKey), Base::SystemException);

        // 两个文件各自都在、也能各自加载，只有配对检查拦得住：这一道是构造里最后一道抛点
        QuicServer::Configuration mismatchedPair;
        mismatchedPair.certificateFile = certificatePath();
        mismatchedPair.privateKeyFile  = std::string(TEST_FIXTURES_DIR) + "/test_ip_key.pem";
        EXPECT_THROW(QuicServer server(loop, mismatchedPair), Base::SystemException);

        // 反复构造失败也要抛，且不留残留：以上三道各来一轮，退出时 LSan 不许报任何东西
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            EXPECT_THROW(QuicServer server(loop, missingCertificate), Base::SystemException);
        }
    }
} // namespace AsynGyanis::Net
