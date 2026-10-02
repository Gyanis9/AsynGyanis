/**
 * @file AcmeAutomationConfig.h
 * @brief 把证书自动化（ACME）接进配置文档：一段 `acme` 读出管理器配置与 DNS-01 口径
 * @author Gyanis
 * @date 2026-09-30
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Base/Config/ConfigValue.h"
#include "Net/Acme/AcmeCertificateManager.h"
#include "Net/Acme/AcmeClient.h"
#include "Net/Acme/AcmeDns01TxtWriter.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace AsynGyanis::Core
{
    class EventLoop;
} // namespace AsynGyanis::Core

namespace AsynGyanis::Net
{
    /// 证书自动化在配置文档里的段名
    inline constexpr std::string_view kAcmeConfigSection = "acme";

    /// DNS-01 的 AccessKey Id 从这条环境变量进；配置文件里没有它的位置
    inline constexpr std::string_view kDnsAccessKeyIdEnvironmentVariable = "ASYN_ACME_DNS_ACCESS_KEY_ID";

    /// DNS-01 的 AccessKey Secret 从这条环境变量进；配置文件里没有它的位置
    inline constexpr std::string_view kDnsAccessKeySecretEnvironmentVariable = "ASYN_ACME_DNS_ACCESS_KEY_SECRET";

    /// `acme.challenge` 接受的两个值（与 AcmeChallengeKind 一一对应，别的写法当场拒）
    inline constexpr std::string_view kAcmeChallengeHttp01 = "http-01";
    inline constexpr std::string_view kAcmeChallengeDns01  = "dns-01";

    /// `acme.dns.provider` 目前唯一有实现的提供方
    inline constexpr std::string_view kAcmeDnsProviderAliyun = "aliyun";

    /**
     * @brief 一段 `acme` 配置读出来的结果
     *
     * @details 刻意**不**自己再造一份设置结构体：`manager` 就是 `AcmeCertificateManager` 直接吃的那份，
     *          再加一层就会有两处默认值要同步（而这类「配置说 A、构造体写 B」的偏差正是最难查的）。
     *          DNS-01 那三项是管理器没有的字段（管理器只认「有没有动作对」），所以单列在这里。
     * @note 凭据不在这里：AccessKey 只从环境变量进，见 `buildDns01TxtWriter`
     */
    struct ASYN_NET_API AcmeAutomationConfiguration
    {
        bool                                  isEnabled{false};                         ///< 整段没写或 enabled=false 时，其余字段保持默认且不会有人去建管理器
        AcmeCertificateManager::Configuration manager{};                                ///< 直接可交给管理器的运维层配置
        AcmeChallengeKind                     challengeKind{AcmeChallengeKind::Http01}; ///< 走哪条自证通道；决定要不要建 TXT 写入动作
        std::string                           dnsProvider{};                            ///< DNS 提供方名（目前只有 "aliyun"）
        std::string                           dnsZoneDomainName{};                      ///< 云解析里的主域名；空 = 由实现按「域名最后两段」推
        std::uint32_t                         dnsRecordTtlSeconds{600U};                ///< 写进去那条 TXT 的 TTL（秒）

        /**
         * @brief 这一份配置要不要走 DNS-01
         */
        [[nodiscard]] bool usesDns01() const noexcept
        {
            return challengeKind == AcmeChallengeKind::Dns01;
        }
    };

    /**
     * @brief 从配置文档里读出 `acme` 段
     * @details 段不存在时交回 `isEnabled=false` 的默认值（旧配置文件不会因为升级而多出一个必填项）。
     *          其余一切不合法都在读配置这一刻抛出，而不是留到第一次签发：证书自动化最常见的现场是
     *          「配置写了但没生效」，而它往往发生在没人手动跑一次签发的那台机器上。
     * @param configurationRoot 整份配置文档（本函数只看 `acme` 段）
     * @return AcmeAutomationConfiguration 读出的配置
     * @throws Base::ConfigValidationException 未知键、类型不对、越界值，以及这几条交叉判据：
     *         开了开关却缺路径或域名、没显式接受服务条款、`challenge` 写了第三种值、
     *         配了 `dns` 子段但走的不是 dns-01（一份没人读的配置）、走 dns-01 但提供方不认识
     */
    [[nodiscard]] ASYN_NET_API AcmeAutomationConfiguration readAcmeConfiguration(const Base::ConfigValue &configurationRoot);

    /**
     * @brief 按配置里的 DNS-01 口径造出 TXT 的发布与撤回动作
     * @details 凭据在这里才进现场：只读 `ASYN_ACME_DNS_ACCESS_KEY_ID` 与 `ASYN_ACME_DNS_ACCESS_KEY_SECRET`
     *          两条环境变量，缺任一条当场拒。配置文件里不给密钥留位置是刻意的——一份能改域名记录的
     *          钥匙进了版本库，等于把域名交出去；命令行上也不行（会进进程列表与 shell 历史）。
     * @param loop 承载出站请求与等待的事件循环
     * @param configuration `readAcmeConfiguration` 的结果（须 `usesDns01()`）
     * @return AcmeDns01TxtWriter 两格都填好的动作对
     * @throws Base::ConfigValidationException 没走 dns-01、提供方不认识、或凭据缺失
     */
    [[nodiscard]] ASYN_NET_API AcmeDns01TxtWriter buildDns01TxtWriter(Core::EventLoop &loop, const AcmeAutomationConfiguration &configuration);

    /**
     * @brief 装配现场才知道的几件事实
     *
     * @details 读取器只回答「这一段写得对不对」，而证书自动化最难查的事故出在两段之间：配置合法、
     *          监听器也在跑，只是签完的那张没人装回线上，或装的时候读的是另一个路径。这几件事只有
     *          把监听器建起来的一方知道，所以交回给调用方填——读取器不该去猜本进程有没有 TLS 口。
     */
    struct ASYN_NET_API AcmeAssemblyFacts
    {
        bool                  hasTlsListener{false};              ///< 本进程里有没有会因这次续期而重装的 TLS 监听器（HTTPS 与 h3 都算）
        bool                  hasPublicPlaintextListener{false};  ///< 有没有公网可达的明文监听器：http-01 的令牌要从那里被机构取走
        bool                  runsMultipleWorkerProcesses{false}; ///< 是不是多 worker 进程形态：签发只归一个进程做，其余进程得自己跟盘
        bool                  picksUpCertificateFromDisk{false};  ///< 本进程会不会在证书文件被别的进程换掉之后自己重装（跟随协程在跑，或本进程就是签发方且就地装回）
        std::filesystem::path listenerCertificateFile{};          ///< 监听器实际加载的证书链路径（reload 按这条原路径重读）
        std::filesystem::path listenerPrivateKeyFile{};           ///< 监听器实际加载的私钥路径
    };

    /**
     * @brief 判一段 acme 配置装配下去之后，线上身份会不会真的跟着换新
     * @details 判据全部是「配了但不生效」那一类形状，而不是配置文件的写法：证书自动化最坏的失败不是
     *          报错，而是磁盘上的证书每月都在换、服务却永远用着那张旧的，而面板上看不出来。因此这四处
     *          一律在启动那一刻拒，交回一句「该改哪里」：
     *          ① 多 worker 进程而本进程不跟盘——签发只该有一个进程做（N 份管理器会各撞一次机构，
     *             速率限制按账户计，不是按进程），而签发进程改的是磁盘：其余进程要靠自己盯住那张文件
     *             的变化才会换身份，没这条通道时它们会永远用着旧的那张；
     *          ② 没有任何 TLS 监听器——续期循环自己也拒绝启动（没有装回的对象），但那一档要到第一次
     *             查到期才出声，太晚；
     *          ③ 走 http-01 却没有公网可达的明文口——机构取不到令牌，每次下单都撞在自证上；
     *          ④ 监听器加载的路径与 acme 的落点不是同一条——`reloadCertificate()` 按原路径重读，
     *             签完的新那张永远装不上去。路径按规范式比对（`./cert.pem` 与 `cert.pem` 是同一条），
     *             规范化做不到时退回逐字比对。
     * @param configuration `readAcmeConfiguration` 的结果
     * @param facts 装配现场的事实
     * @return std::expected<void, std::string> 装得回去时成立；失败值是给人读的一句原因与出路。
     *         `isEnabled=false` 直接成立——没开这一项就没有可判的装配
     */
    [[nodiscard]] ASYN_NET_API std::expected<void, std::string> validateAcmeAssembly(const AcmeAutomationConfiguration &configuration, const AcmeAssemblyFacts &facts);
} // namespace AsynGyanis::Net
