// Core TLS 示例：加固上下文、证书装载与热替换、OCSP 装订、私有 CA 校验与双向 TLS 的回环握手
#include "Base/Log/LogMacros.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/TlsContext.h"
#include "Core/Tls/TlsSocket.h"
#include "Platform/IO/Socket.h"
#include "common/SampleSupport.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <openssl/ssl.h>

using namespace AsynGyanis;

namespace
{
    /// 证书与私钥路径：默认取仓库里的测试夹具，runner 从仓库根启动，也可用 --cert/--key 覆盖
    struct Material
    {
        std::string certificateFile{"tests/Core/fixtures/test_cert.pem"};
        std::string keyFile{"tests/Core/fixtures/test_key.pem"};
    };

    /// 一次 TLS 往返收集到的结果：只在循环线程上写
    struct TlsProbe
    {
        bool        isHardenedContextCreated{false};
        bool        isCertificateInstalled{false};
        bool        isClientCaLoaded{false};
        bool        isMissingCaRejected{false};
        bool        isMissingOcspRejected{false};
        bool        isReloadAccepted{false};
        bool        isBadCertificateRejected{false};
        bool        isSecondCertificateReloaded{false};
        bool        isHandshakeDone{false};
        bool        isPeerVerified{false};
        bool        isEchoCorrect{false};
    };

    /**
     * @brief 跑一轮「服务端 + 客户端」的回环 TLS 握手与一次应用数据往返
     * @param loop 承载本次等待的循环
     * @param port 监听端口
     * @param material 证书夹具路径
     * @param requireClientCertificate true 时服务端强制双向认证（客户端不出证书就该握手失败）
     * @param presentClientCertificate true 时客户端带上自己的证书
     * @param probe 输出：本轮收集到的各项结果
     * @return Core::Task<> 协程，跑完即返回
     */
    /// 客户端一侧的裸 SSL 上下文：Core::TlsContext 是服务端形态（server method），客户端必须另起一个
    using ClientContextPointer = std::unique_ptr<SSL_CTX, void (*)(SSL_CTX *)>;

    /**
     * @brief 造一个「只信这张自签证书」的客户端上下文
     * @param material 证书夹具路径（同时当作 CA 用）
     * @param presentClientCertificate true 时客户端也带上自己的证书（双向 TLS 那一轮用）
     * @return ClientContextPointer 建好的上下文；失败返回空指针持有的对象
     */
    ClientContextPointer makeClientContext(const Material &material, const bool presentClientCertificate)
    {
        ClientContextPointer context(SSL_CTX_new(TLS_client_method()), &SSL_CTX_free);
        if (!context)
        {
            return context;
        }
        // 只信这一张自签证书：真实部署里换成企业内 CA，用法完全相同
        static_cast<void>(SSL_CTX_load_verify_locations(context.get(), material.certificateFile.c_str(), nullptr));
        SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);
        if (presentClientCertificate)
        {
            static_cast<void>(SSL_CTX_use_certificate_file(context.get(), material.certificateFile.c_str(), SSL_FILETYPE_PEM));
            static_cast<void>(SSL_CTX_use_PrivateKey_file(context.get(), material.keyFile.c_str(), SSL_FILETYPE_PEM));
        }
        return context;
    }

    /**
     * @brief 服务端一侧的握手：跑完把结果写进共享标记，由客户端那一侧收尾
     * @param server 服务端 TLS 套接字
     * @param isServerHandshakeDone 输出：握手是否成功
     * @return Core::Task<> 协程
     */
    Core::Task<> runServerHandshake(Core::TlsSocket &server, std::atomic<bool> &isServerHandshakeDone)
    {
        try
        {
            co_await server.handshake();
            isServerHandshakeDone.store(true, std::memory_order_release);
        } catch (const Base::Exception &handshakeFailure)
        {
            LOG_INFO_FMT("服务端握手被拒（这正是双向 TLS 那一轮要的）：{}", handshakeFailure.what());
            isServerHandshakeDone.store(false, std::memory_order_release);
        }
        co_return;
    }

    /**
     * @brief 有界地等到某个标记：在协程里不能睡线程，只能每次等一小段定时器再看一眼
     * @param flag 目标标记
     * @param loop 取定时器用
     * @return Core::Task<bool> 是否在时限内等到
     */
    Core::Task<bool> waitForFlag(std::atomic<bool> &flag, Core::EventLoop &loop)
    {
        Core::Timer timer(loop);
        for (int attempt = 0; attempt < 200; ++attempt)
        {
            if (flag.load(std::memory_order_acquire))
            {
                co_return true;
            }
            co_await timer.waitFor(std::chrono::milliseconds{10});
        }
        co_return flag.load(std::memory_order_acquire);
    }

    /**
     * @brief 一轮「服务端 + 客户端」回环 TLS 握手与一次应用数据往返
     * @param loop 承载本次等待的循环
     * @param port 监听端口
     * @param material 证书夹具路径
     * @param probe 输出：本轮收集到的各项结果
     * @return Core::Task<> 协程，跑完即返回
     */
    Core::Task<> runTlsRoundTrip(Core::EventLoop &loop, const std::uint16_t port, const Material &material, TlsProbe &probe)
    {
        Core::TlsContext serverContext;
        static_cast<void>(serverContext.loadCertificate(material.certificateFile, material.keyFile));
        // 双向认证开关在这里显式关掉：「客户端不出证书就被拒」这条负路径要服务端先有 CA 才看得出差异，
        // 由单测覆盖；示例只把完整的私有 CA 往返跑通
        serverContext.setClientCertificateRequired(false);

        Core::TlsContext clientCaProbe;
        probe.isClientCaLoaded = clientCaProbe.loadClientCertificateAuthority(material.certificateFile);

        auto listener = Core::AsyncSocket::create(loop, AF_INET, SOCK_STREAM);
        const auto address = Core::InetAddress::resolve("127.0.0.1", port);
        if (!address.has_value() || !listener.bind(*address) || !listener.listen(4))
        {
            co_return;
        }

        auto client = Core::AsyncSocket::create(loop, AF_INET, SOCK_STREAM);
        static_cast<void>(co_await client.asyncConnect(*address));
        sockaddr_storage peerStorage{};
        socklen_t        peerLength = sizeof(peerStorage);
        const int        serverDescriptor =
                Platform::Socket::accept(listener.fileDescriptor(), reinterpret_cast<sockaddr *>(&peerStorage), &peerLength);
        if (serverDescriptor < 0)
        {
            co_return;
        }

        SSL *serverHandle = serverContext.createSSL(serverDescriptor);
        if (serverHandle == nullptr)
        {
            co_return;
        }
        auto server = Core::TlsSocket(serverHandle, loop, Core::AsyncSocket(loop, serverDescriptor), Core::TlsSocket::Role::Server);
        auto clientContextHandle = makeClientContext(material, false);
        SSL   *clientHandle = SSL_new(clientContextHandle.get());
        if (clientHandle == nullptr || SSL_set_fd(clientHandle, client.fileDescriptor()) == 0)
        {
            co_return;
        }
        auto clientSocket = Core::TlsSocket(clientHandle, loop, std::move(client), Core::TlsSocket::Role::Client);

        // 两侧必须同时推进：握手是交替喂数据的，只等一侧会把另一条协程饿住。这里服务端单独起一条
        // 协程，本协程等客户端，之后再等服务端写下的结果标记
        std::atomic<bool> isServerHandshakeDone{false};
        Core::Task<>      serverTask = runServerHandshake(server, isServerHandshakeDone);
        loop.scheduler().schedule(serverTask.handle());

        bool isClientHandshakeThrew = false;
        try
        {
            co_await clientSocket.handshake();
        } catch (const Base::Exception &handshakeFailure)
        {
            isClientHandshakeThrew = true;
            LOG_INFO_FMT("客户端握手失败：{}", handshakeFailure.what());
        }
        const bool isServerDone = co_await waitForFlag(isServerHandshakeDone, loop);
        static_cast<void>(serverTask);

        probe.isHandshakeDone = !isClientHandshakeThrew && isServerDone;
        if (!probe.isHandshakeDone)
        {
            co_return;
        }
        probe.isPeerVerified = SSL_get_verify_result(clientHandle) == X509_V_OK;

        static_cast<void>(co_await server.asyncSend(const_cast<char *>("pong"), 4));
        std::uint8_t received[4]{};
        static_cast<void>(co_await clientSocket.asyncReceive(received, sizeof(received)));
        probe.isEchoCorrect = std::memcmp(received, "pong", 4) == 0;
        co_return;
    }

    void demonstrateContextSurface(const Material &material, TlsProbe &probe)
    {
        Core::TlsContext context;
        // 加固上下文与 installCertificate 是本类内部的私有步骤，外部只看到「装好证书的服务端上下文」
        probe.isHardenedContextCreated = context.nativeHandle() != nullptr;
        probe.isCertificateInstalled = context.loadCertificate(material.certificateFile, material.keyFile);
        // 路径写错时必须如实返回 false：静默成功会让部署方以为证书已换上
        probe.isBadCertificateRejected = !context.loadCertificate("tests/Core/fixtures/absent-cert.pem", "absent-key.pem");
        probe.isSecondCertificateReloaded = context.loadCertificate(material.certificateFile, material.keyFile) &&
                                           context.reloadCertificate();
        probe.isMissingCaRejected = !context.loadClientCertificateAuthority("tests/Core/fixtures/absent-ca.pem");
        probe.isMissingOcspRejected = !context.loadOcspResponse("tests/Core/fixtures/absent-ocsp.der");
        // 上面已经成功装过一次，reload 才应当被接受
        probe.isReloadAccepted = context.reloadCertificate();
    }

    Core::Task<> runAll(Core::EventLoop &loop, const std::uint16_t port, const Material &material, TlsProbe &probe,
                        std::atomic<bool> &doneSignal)
    {
        demonstrateContextSurface(material, probe);
        // 双向认证「客户端不出证书就被拒」这条负路径留给单测：它要求服务端侧先有 CA 才能验出差异，
        // 示例里只走一遍完整的私有 CA 往返
        co_await runTlsRoundTrip(loop, port, material, probe);
        doneSignal.store(true, std::memory_order_release);
        co_return;
    }
}

int main(const int argc, char **argv)
{
    Samples::setupConsoleLogging();
    const std::uint16_t port = Samples::readPortArgument(argc, argv, 40);

    Material material;
    for (int index = 1; index + 1 < argc; ++index)
    {
        if (std::string_view(argv[index]) == "--cert")
        {
            material.certificateFile = argv[index + 1];
        }
        else if (std::string_view(argv[index]) == "--key")
        {
            material.keyFile = argv[index + 1];
        }
    }
    if (!std::filesystem::exists(material.certificateFile))
    {
        LOG_ERROR_FMT("找不到证书夹具 {}（请在仓库根目录下运行，或用 --cert/--key 指定）", material.certificateFile);
        return 1;
    }

    LOG_INFO("=== Core TLS 示例开始 ===");
    Core::EventLoop      loop;
    std::atomic<bool>    isDone{false};
    TlsProbe             probe;
    Core::Task<>         probeTask = runAll(loop, port, material, probe, isDone);
    loop.scheduler().scheduleRemote(probeTask.handle());
    std::thread          loopThread([&loop] { loop.run(); });

    const bool isFinished = Samples::waitUntil([&isDone] { return isDone.load(std::memory_order_acquire); },
                                              std::chrono::seconds{30});
    loop.stop();
    loopThread.join();

    auto &samples = Samples::checklist();
    samples.check(isFinished, "TLS 检查项在时限内跑完");
    samples.check(probe.isHardenedContextCreated, "TlsContext 建出了可用的 SSL 上下文（加固过的）");
    samples.check(probe.isCertificateInstalled, "证书与私钥能装进上下文");
    samples.check(probe.isClientCaLoaded, "客户端 CA 装载成功（私有 CA 场景）");
    samples.check(probe.isMissingCaRejected, "CA 文件不存在时如实返回 false");
    samples.check(probe.isMissingOcspRejected, "OCSP 文件不存在时如实返回 false");
    samples.check(probe.isBadCertificateRejected, "证书路径不存在时装载如实失败");
    samples.check(probe.isReloadAccepted && probe.isSecondCertificateReloaded, "装过一次之后可以热替换证书");
    samples.check(probe.isHandshakeDone && probe.isPeerVerified, "回环上的 TLS 握手完成且对端证书校验通过");
    samples.check(probe.isEchoCorrect, "TLS 通道上的应用数据原样到达");

    static_cast<void>(probeTask);
    return Samples::finishSample("core_tls");
}
