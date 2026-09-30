// ACME 真签发探针的被测端：对着一个**不是本仓库写的** ACME 服务端把整台状态机走到底，
// 把每一步的结果打成 stdout 事件行，由 scripts/acme_pebble_cross_check.sh 判定。
//
// 为什么要有它：进程内的桩机构虽然按「重建公钥再验签」这条独立路径判，但它与实现同源、同一批人维护，
// 两边对 RFC 8555 的同一种误读会一起绿；真机构那条按环境变量门控的用例只走到「取目录 + 建号」，
// 因为真签发要一台公网可达、域名又解析到本机的机器——回归环境给不了。Pebble 是 Let's Encrypt
// 官方那套 ACME 测试服务端（Go 写的独立实现），它把 HTTP-01 的取令牌端口配成 5002，于是回环就能走通：
// 域名用 127.0.0.1.sslip.io 这类通配解析，机构的校验器解析回来的就是本机。
// 这一步能证明的是「另一套实现接受我们的 JWS、接受我们的 CSR 并真的签出一张链」，
// 加上脚本用 openssl 拿 Pebble 的根去验那张链；它**不**证明公网可达、不证明浏览器信任。
//
// 输出协议（每行一条，无缓冲立即落盘，供脚本读取）：
//   DIRECTORY <url>              实际用的机构目录
//   CHALLENGE <http-01|dns-01> PROVIDER <aliyun|无> ZONE <主域名或 derived> TTL <秒或 ->
//                                本轮真的按哪一种自证在跑（DNS-01 档必须看到这行才说明开关被消费了）
//   BINDING <ip>:<port>          令牌服务真正绑上的地址（默认 127.0.0.1；打真机构时必须看到 0.0.0.0）
//   LISTENING <port>             自证令牌服务的实际端口（内核发布之后才打；DNS-01 档没有这两行）
//   ISSUED <到期epoch> IDENTITY <链的sha256前12位>
//   RENEWED <到期epoch> IDENTITY <...>   第二轮真的重签了（续期路径通）
//   SKIPPED                      第二轮判断「够用」所以没去打扰机构（额度未耗尽那条出口）
//   RELOAD_CALLS <n>             装回服务的回调被调了几次
//   ISSUANCES <n>                status() 记的签发次数
//   FAILED <第几轮> <原因>        任何一条出口失败都打这行并以非 0 退出
#include "Core/Crypto/Digest.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Acme/AcmeAliyunDns01TxtWriter.h"
#include "Net/Acme/AcmeCertificateManager.h"
#include "Net/Acme/AcmeDns01TxtWriter.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/Router.h"
#include "Platform/IO/FileContents.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using AsynGyanis::Core::EventLoop;
    using AsynGyanis::Core::Timer;
    using AsynGyanis::Net::AcmeCertificateManager;
    using AsynGyanis::Net::AcmeDns01TxtWriter;
    using AsynGyanis::Net::AliyunDns01Configuration;
    using AsynGyanis::Net::HttpServer;

    /**
     * @brief 命令行选项
     */
    struct Options
    {
        std::string   directoryUrl;               ///< 机构目录（Pebble 是 https://127.0.0.1:14000/dir）
        std::string   domain;                     ///< 要覆盖的域名（回环跑法是 127.0.0.1.sslip.io，`*.` 开头就是通配符）
        std::uint16_t challengePort{};            ///< 自证令牌的明文端口，必须与机构配置里的 httpPort 一致
        std::string   challengeBind{"127.0.0.1"}; ///< 令牌服务的绑定地址：回环跑法保持默认，真机构从公网来取得填 0.0.0.0
        std::string   stateDirectory;             ///< 密钥、证书与账户状态的落点
        std::string   contact{"mailto:acme-probe@example.com"};
        std::string   challengeKind{"http-01"}; ///< 走哪一种自证：dns-01 时不需要任何入站通路
        std::string   dnsZone;                  ///< 云解析里的主域名；留空由实现按「域名最后两段」推
        std::uint32_t dnsTtl{600U};             ///< 写进去那条 TXT 的 TTL（秒）
        bool          forceRenewal{false};      ///< 把续期窗口撑到十年，逼出第二轮的真重签
    };

    /**
     * @brief 交一条事件行出去：无缓冲，脚本能在进程退出前就读到
     */
    void emit(const std::string &line)
    {
        std::cout << line << '\n';
        std::cout.flush();
    }

    /**
     * @brief 取一个必给的开关值
     */
    [[nodiscard]] std::string needValue(int argc, char **argv, int &index, const std::string &flag)
    {
        if (index + 1 >= argc)
        {
            std::cerr << "开关 " << flag << " 缺参数\n";
            std::exit(2);
        }
        return argv[++index];
    }

    /**
     * @brief 解析命令行
     */
    [[nodiscard]] Options parseArguments(const int argc, char **argv)
    {
        Options options;
        for (int index = 1; index < argc; ++index)
        {
            const std::string flag = argv[index];
            if (flag == "--directory-url")
            {
                options.directoryUrl = needValue(argc, argv, index, flag);
            } else if (flag == "--domain")
            {
                options.domain = needValue(argc, argv, index, flag);
            } else if (flag == "--challenge-port")
            {
                options.challengePort = static_cast<std::uint16_t>(std::stoi(needValue(argc, argv, index, flag)));
            } else if (flag == "--challenge-bind")
            {
                options.challengeBind = needValue(argc, argv, index, flag);
            } else if (flag == "--state-dir")
            {
                options.stateDirectory = needValue(argc, argv, index, flag);
            } else if (flag == "--contact")
            {
                options.contact = needValue(argc, argv, index, flag);
            } else if (flag == "--challenge")
            {
                options.challengeKind = needValue(argc, argv, index, flag);
            } else if (flag == "--dns-zone")
            {
                options.dnsZone = needValue(argc, argv, index, flag);
            } else if (flag == "--dns-ttl")
            {
                options.dnsTtl = static_cast<std::uint32_t>(std::stoul(needValue(argc, argv, index, flag)));
            } else if (flag == "--renew")
            {
                options.forceRenewal = true;
            } else
            {
                std::cerr << "未知参数：" << flag
                          << "\n用法：acme_issuance_probe --directory-url <url> --domain <name> --state-dir <dir> "
                             "[--challenge http-01|dns-01] [--challenge-port <n>] [--challenge-bind <ip，默认 127.0.0.1>] "
                             "[--dns-zone <主域名>] [--dns-ttl <秒>] [--contact mailto:...] [--renew]\n";
                std::exit(2);
            }
        }
        if (options.directoryUrl.empty() || options.domain.empty() || options.stateDirectory.empty())
        {
            std::cerr << "--directory-url / --domain / --state-dir 三个都必须给\n";
            std::exit(2);
        }
        // 自证种类只认这两个值：拼错的第三种不能悄悄按默认跑，那样「以为在测 dns-01、实际测的是 http-01」
        // 会把一条没走过的通路读成走过
        if (options.challengeKind != "http-01" && options.challengeKind != "dns-01")
        {
            std::cerr << "--challenge 只接受 http-01 或 dns-01，给的是：" << options.challengeKind << "\n";
            std::exit(2);
        }
        if (options.challengeKind == "http-01" && options.challengePort == 0U)
        {
            std::cerr << "走 http-01 时 --challenge-port 必须给（机构按那个端口取令牌）， dns-01 才不需要入站通路\n";
            std::exit(2);
        }
        return options;
    }

    /**
     * @brief 读整份证书链，算出一个能比对「换没换张证书」的身份串
     * @details 用链的 SHA-256 前 12 位而不是序列号：Pebble 每次签发都给新序列号，而我们要判的是
     *          「第二轮真的拿到了一份不同的字节流」，摘要更直接，也不必为探针再引一层 X509 解析
     */
    [[nodiscard]] std::string chainIdentity(const std::filesystem::path &certificateFile)
    {
        std::error_code      sizeFailure;
        const std::uintmax_t size = std::filesystem::file_size(certificateFile, sizeFailure);
        if (static_cast<bool>(sizeFailure))
        {
            return "unreadable";
        }
        const auto contents = AsynGyanis::Platform::readFileContents(certificateFile, 0U, static_cast<std::size_t>(size));
        if (!contents.has_value())
        {
            return "unreadable";
        }
        return AsynGyanis::Core::Digest::sha256Hex(*contents).substr(0, 12);
    }

    /**
     * @brief 驱动协程：等端口发布 → 签一轮 → 再签一轮 → 叫停循环
     * @details 每条出口都要 stop()，漏一条就把整个探针挂在那儿没人推进的循环上。
     *          dns-01 档没有那台明文服务（不需要入站通路），此时端口那一段整个跳过。
     * @param ok 由本协程写：任何一条 FAILED 出口都置 false，main 据此决定退出码——
     *           「打了 FAILED 却退 0」对下游消费者是个陷阱
     */
    AsynGyanis::Core::Task<void> drive(EventLoop &loop, AcmeCertificateManager &manager, std::optional<HttpServer> &challengeServer, bool &ok)
    {
        Timer timer(loop);
        if (challengeServer.has_value())
        {
            // 端口由接受协程发布，所以这里要等它，而不是假设 bind 成功就等于端口可见
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
            while (challengeServer->listeningPort() == 0U)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    ok = false;
                    emit("FAILED setup 自证服务的监听端口在 10 秒内没发布出来");
                    loop.stop();
                    co_return;
                }
                co_await timer.waitFor(std::chrono::milliseconds{5});
            }
            emit(std::format("LISTENING {}", challengeServer->listeningPort()));
        }

        const auto first = co_await manager.issueIfRequired();
        if (!first.has_value())
        {
            ok = false;
            emit(std::format("FAILED 1 {}", first.error().message));
            loop.stop();
            co_return;
        }
        emit(std::format("ISSUED {} IDENTITY {}", std::chrono::duration_cast<std::chrono::seconds>(first->expiry.time_since_epoch()).count(),
                         chainIdentity(first->certificateFile)));

        const auto second = co_await manager.issueIfRequired();
        if (!second.has_value())
        {
            ok = false;
            emit(std::format("FAILED 2 {}", second.error().message));
            loop.stop();
            co_return;
        }
        // 第二轮看的是签发计数：续期窗口够宽就一定重签（打 RENEWED），窄到还能用就该一次都不签（打 SKIPPED）
        const auto status = manager.status();
        if (status.issuanceCount >= 2U)
        {
            emit(std::format("RENEWED {} IDENTITY {}", std::chrono::duration_cast<std::chrono::seconds>(second->expiry.time_since_epoch()).count(),
                             chainIdentity(second->certificateFile)));
        } else
        {
            emit("SKIPPED");
        }

        emit(std::format("ISSUANCES {}", status.issuanceCount));
        emit("DONE");
        loop.stop();
        co_return;
    }
} // namespace

/**
 * @brief 入口：起一台明文自证服务 + 一个证书管理器，连走两轮签发
 */
int main(const int argc, char **argv)
{
    // 行缓冲在管道里会变成全缓冲，脚本要的是「事件一出就能读」
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const Options options = parseArguments(argc, argv);

    std::error_code ignore;
    std::filesystem::create_directories(options.stateDirectory, ignore);

    AcmeCertificateManager::Configuration configuration;
    configuration.domainNames         = {options.domain};
    configuration.certificateFile     = std::filesystem::path(options.stateDirectory) / "chain.pem";
    configuration.privateKeyFile      = std::filesystem::path(options.stateDirectory) / "domain-key.pem";
    configuration.accountKeyFile      = std::filesystem::path(options.stateDirectory) / "account-key.pem";
    configuration.accountStateFile    = std::filesystem::path(options.stateDirectory) / "account.json";
    configuration.directoryUrl        = options.directoryUrl;
    configuration.contactEmailAddress = options.contact;
    // 探针是操作者显式启动的，接受条款这个动作由启动它的人完成；这里不替谁默认同意
    configuration.isTermsOfServiceAccepted = true;
    configuration.issuanceTimeout          = std::chrono::seconds{120};
    configuration.challengePollInterval    = std::chrono::milliseconds{500};
    configuration.minimumRetryInterval     = std::chrono::seconds{1};
    // 续期窗口决定第二轮签不签：宽到十年就一定签得出第二张，窄到一天则第一张还剩有效期、不该再打扰机构
    configuration.renewBeforeExpiry = options.forceRenewal ? std::chrono::hours{24 * 3650} : std::chrono::hours{24};

    emit(std::format("DIRECTORY {}", options.directoryUrl));

    EventLoop loop;

    const bool isDns01 = options.challengeKind == "dns-01";

    // DNS-01 的动作对：凭据只从环境读。命令行上的参数会进 ps 与 shell 历史，配置文件会进版本库，
    // 而这对钥匙能改域名记录，等于把域名交出去
    AcmeDns01TxtWriter dnsWriter;
    if (isDns01)
    {
        const char *const keyId     = std::getenv("ASYN_ACME_DNS_ACCESS_KEY_ID");
        const char *const keySecret = std::getenv("ASYN_ACME_DNS_ACCESS_KEY_SECRET");
        if (keyId == nullptr || keySecret == nullptr || *keyId == '\0' || *keySecret == '\0')
        {
            std::cerr << "走 dns-01 需要环境变量 ASYN_ACME_DNS_ACCESS_KEY_ID 与 ASYN_ACME_DNS_ACCESS_KEY_SECRET，"
                         "两者都非空才起步：缺任一条就没法把 TXT 写进云解析，与其跑到一半再失败，不如现在拒。\n";
            std::exit(2);
        }
        AliyunDns01Configuration dns;
        dns.accessKeyId      = keyId;
        dns.accessKeySecret  = keySecret;
        dns.zoneDomainName   = options.dnsZone;
        dns.recordTtlSeconds = options.dnsTtl;
        dnsWriter            = AsynGyanis::Net::makeAliyunDns01TxtWriter(loop, std::move(dns));
    }
    // 这行是「新档位真的被消费了」的标记：脚本先断言它再去判结果，
    // 否则一个没被读到的 --challenge 会按默认的 http-01 跑完全程并交出看起来正确的证据
    emit(std::format("CHALLENGE {} PROVIDER {} ZONE {} TTL {}", options.challengeKind, isDns01 ? "aliyun" : "-",
                     isDns01 ? (options.dnsZone.empty() ? "derived" : options.dnsZone) : "-", isDns01 ? std::to_string(options.dnsTtl) : "-"));

    int reloadCalls = 0;
    // 装回服务的动作在这里是记账：签发成功后回调必须被调到，否则「磁盘新、线上旧」这条失败模式没人拦
    AcmeCertificateManager manager(
            loop, configuration,
            [&reloadCalls]() -> std::expected<void, std::string>
            {
                ++reloadCalls;
                return {};
            },
            std::move(dnsWriter));

    // dns-01 不需要任何入站通路；http-01 才起那台明文服务
    std::optional<HttpServer>                   challengeServer;
    std::optional<AsynGyanis::Core::Task<void>> acceptTask;
    if (!isDns01)
    {
        // 绑定地址只接受 IP 字面量：解析失败若悄悄退回默认值，就会把「只想绑回环」变成「绑到全网卡」，
        // 那是比失败更糟的结果，所以这里直接拒掉退出。
        const std::optional<AsynGyanis::Core::InetAddress> challengeAddress = AsynGyanis::Core::InetAddress::parseLiteral(options.challengeBind, options.challengePort);
        if (!challengeAddress.has_value())
        {
            std::cerr << "--challenge-bind 不是可解析的 IP 字面量：" << options.challengeBind << "\n";
            std::exit(2);
        }
        emit(std::format("BINDING {}:{}", options.challengeBind, options.challengePort));

        challengeServer.emplace(loop, *challengeAddress);
        manager.registerChallengeRoutes(challengeServer->router());

        // 协程帧由本函数持有到 loop.run() 之后：Task 的析构是无条件 destroy()，帧提前析构等于在跑的时候抽走它
        acceptTask.emplace(challengeServer->start());
        loop.scheduler().schedule(acceptTask->handle());
    }

    std::optional<AsynGyanis::Core::Task<void>> driverTask;
    bool                                        isWholeFlowValid = true;
    driverTask.emplace(drive(loop, manager, challengeServer, isWholeFlowValid));
    loop.scheduler().schedule(driverTask->handle());

    loop.run();
    driverTask.reset();
    acceptTask.reset();

    // 回调记账读给脚本：两次签发（--renew）应当听到两次装回
    emit(std::format("LOCAL_RELOAD_COUNT {}", reloadCalls));
    return isWholeFlowValid ? 0 : 1;
}
