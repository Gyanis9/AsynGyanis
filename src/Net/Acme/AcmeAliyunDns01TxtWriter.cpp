#include "Net/Acme/AcmeAliyunDns01TxtWriter.h"

#include "Base/Coding/Base64.h"
#include "Base/Config/ConfigValue.h"
#include "Base/Log/LogMacros.h"
#include "Core/Crypto/Digest.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Net/Http/Client/HttpClient.h"
#include "Platform/System/PlatformTime.h"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    std::string aliyunPercentEncode(const std::string_view value)
    {
        std::string encoded;
        encoded.reserve(value.size());
        for (const unsigned char character: value)
        {
            const bool isUnreserved = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
                                      character == '-' || character == '_' || character == '.' || character == '~';
            if (isUnreserved)
            {
                encoded += static_cast<char>(character);
            } else
            {
                encoded += std::format("%{:02X}", character);
            }
        }
        return encoded;
    }

    std::string aliyunRpcSignature(const std::string_view httpMethod, const std::map<std::string, std::string> &parameters, const std::string_view accessKeySecret)
    {
        // 参数表按名字的 ASCII 序拼串：map 本身就是这个序，不需要再排一次
        std::string canonicalQuery;
        for (const auto &field: parameters)
        {
            if (!canonicalQuery.empty())
            {
                canonicalQuery += '&';
            }
            canonicalQuery += aliyunPercentEncode(field.first);
            canonicalQuery += '=';
            canonicalQuery += aliyunPercentEncode(field.second);
        }

        const std::string stringToSign = std::string(httpMethod) + "&" + aliyunPercentEncode("/") + "&" + aliyunPercentEncode(canonicalQuery);
        // 密钥是「Secret + &」而不是 Secret：少那个 & 会稳定地得到 SignatureDoesNotMatch，
        // 而服务端不会告诉你是哪一段拼错了
        const std::string             signingKey = std::string(accessKeySecret) + "&";
        const Core::Digest::Sha1Value digest     = Core::Digest::hmacSha1(signingKey, stringToSign);
        return Base::base64Encode(std::string_view(reinterpret_cast<const char *>(digest.data()), digest.size()));
    }

    std::chrono::milliseconds aliyunRewriteQuietPeriod(const std::chrono::milliseconds sinceWithdrawal, const std::uint32_t recordTtlSeconds)
    {
        // 负数（调用方把时钟读反了）按「刚撤完」处置：这里的下限是宁可多等，不要让同名重写抢跑
        const std::chrono::milliseconds elapsed = std::max(sinceWithdrawal, std::chrono::milliseconds::zero());
        if (elapsed >= kAliyunMaximumRewriteQuiet)
        {
            return std::chrono::milliseconds::zero();
        }
        const std::chrono::milliseconds ttl{std::chrono::seconds{recordTtlSeconds}};
        // 等满是「旧答案过期」的最低要求，上限只是挡住配错的天量 TTL
        const std::chrono::milliseconds remaining = std::max(ttl - elapsed, std::chrono::milliseconds::zero());
        return std::min(remaining, kAliyunMaximumRewriteQuiet - elapsed);
    }

    namespace
    {
        /// 云解析的中心化接入点（不在某个 region 上，全球同一个域名）
        constexpr std::string_view kEndpointUrl = "https://alidns.aliyuncs.com/";
        constexpr std::string_view kApiVersion  = "2015-01-09";

        /// 单次控制面调用的时限：这是管理接口而不是数据面，慢的时候等下去没有意义
        constexpr std::chrono::milliseconds kRequestTimeout{10000};

        /// 写完之后确认「控制面查得到这条」的轮询节奏与上限
        constexpr std::chrono::milliseconds kVisibilityPollInterval{2000};
        constexpr std::chrono::milliseconds kVisibilityTimeout{60000};

        /// 一次查询回多少条记录：按 Type=TXT 过滤之后正常域名就只有几条，取上限避免分页
        constexpr std::uint32_t kQueryPageSize = 500U;

        /**
         * @brief 一条记录在云解析里的落点：主域名 + 主机记录
         */
        struct DnsRecordTarget
        {
            std::string zone; ///< 云解析里那条主域名，如 `gyanis.space`
            std::string rr;   ///< 主机记录（相对主域名的那一段），如 `_acme-challenge`
        };

        [[nodiscard]] char toLowerAscii(const char character) noexcept
        {
            return (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character;
        }

        /**
         * @brief 判断 recordName 是否落在 zone 里（DNS 的名字不区分大小写）
         */
        [[nodiscard]] bool isWithinZone(const std::string_view recordName, const std::string_view zone) noexcept
        {
            if (zone.empty() || recordName.size() <= zone.size() + 1U)
            {
                return false;
            }
            const std::string_view tail = recordName.substr(recordName.size() - zone.size());
            if (!std::equal(tail.begin(), tail.end(), zone.begin(), [](const char left, const char right) { return toLowerAscii(left) == toLowerAscii(right); }))
            {
                return false;
            }
            return recordName[recordName.size() - zone.size() - 1U] == '.';
        }

        /**
         * @brief 把 `_acme-challenge.<域名>` 折成「主域名 + 主机记录」
         * @details 云解析的 AddDomainRecord 不收完整名字，它要的是「哪条主域名下的哪段主机记录」。
         *          配了主域名就照配的来；没配就取最后两段当主域名——绝大多数注册域名就是这个形状，
         *          而猜错的那一类（主域名本身是三级，如托管在云解析里的 sub.example.com）
         *          会在错误文案里点名让用户去配，不会静默写到别的区里去
         */
        [[nodiscard]] std::optional<DnsRecordTarget> splitRecordName(const std::string_view recordName, const std::string_view configuredZone)
        {
            if (!configuredZone.empty())
            {
                if (!isWithinZone(recordName, configuredZone))
                {
                    return std::nullopt;
                }
                return DnsRecordTarget{std::string(configuredZone), std::string(recordName.substr(0, recordName.size() - configuredZone.size() - 1U))};
            }

            const std::size_t lastDot = recordName.rfind('.');
            if (lastDot == std::string_view::npos || lastDot == 0U)
            {
                return std::nullopt;
            }
            const std::size_t previousDot = recordName.rfind('.', lastDot - 1U);
            if (previousDot == std::string_view::npos)
            {
                return std::nullopt;
            }
            return DnsRecordTarget{std::string(recordName.substr(previousDot + 1U)), std::string(recordName.substr(0, previousDot))};
        }

        /**
         * @brief 阿里云 RPC 风格的 UTC 时间戳：yyyy-MM-ddTHH:mm:ssZ
         * @details 走 PlatformTime 而不交给 C 库的 gmtime：两侧的 gmtime_s/gmtime_r 参数顺序相反，
         *          且 MSVC 那份会把 1970 年之前整个拒掉（见该类注释）
         */
        [[nodiscard]] std::string utcTimestamp()
        {
            const Platform::UtcTimeFields fields = Platform::PlatformTime::utcTime(std::time(nullptr));
            return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}Z", fields.year, fields.month, fields.day, fields.hour, fields.minute, fields.second);
        }

        /**
         * @brief 一次请求一个、不许重放的随机串
         * @details 云解析用它挡重放；重复的 nonce 会被判 InvalidSignatureNonce。RAND_bytes 只在熵源
         *          坏掉时失败，那种情况下退到「时钟 + 计数」也比抛出好：这条通路在协程里，抛出等于
         *          把签发流程的异常通道绕过
         */
        [[nodiscard]] std::string makeSignatureNonce()
        {
            std::array<unsigned char, 16> bytes{};
            if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
            {
                static std::atomic<std::uint64_t> fallbackCounter{0U};
                const std::uint64_t               counter = fallbackCounter.fetch_add(1U, std::memory_order_relaxed);
                const std::uint64_t               nanosecond =
                        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
                const std::string text = std::format("{:016x}{:016x}", nanosecond, counter);
                return text;
            }
            return Base::base64Encode(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
        }

        /**
         * @brief 云解析的调用细节：签名、发请求、把两种应答读成我们要的形状
         */
        class AlidnsClient
        {
        public:
            AlidnsClient(Core::EventLoop &loop, AliyunDns01Configuration configuration) : m_loop(loop), m_configuration(std::move(configuration))
            {
            }

            /**
             * @brief 写入这条 TXT，并确认控制面已经查得到它
             * @param recordName `_acme-challenge.<域名>` 的完整名字
             * @param value      TXT 正文（keyAuthorization 的摘要）
             * @return Core::Task<std::expected<void, std::string>> 成功即可让机构去取；失败带中文原因
             */
            Core::Task<std::expected<void, std::string>> publish(std::string recordName, std::string value)
            {
                const std::optional<DnsRecordTarget> target = splitRecordName(recordName, m_configuration.zoneDomainName);
                if (!target.has_value())
                {
                    co_return std::unexpected(zoneSplitFailure(recordName));
                }

                // 同一个名字刚撤过就要重写：机构那侧还按旧记录的 TTL 缓存着上一条答案，等不满就去自证，
                // 读到的还是上一条——一张单里基础域名与它的通配符正是这种「同名两个答案」的形状
                if (const std::chrono::milliseconds quiet = quietPeriodFor(recordName); quiet > std::chrono::milliseconds::zero())
                {
                    LOG_INFO_FMT("AcmeAliyunDns01: {} 刚撤过一条 TXT，同名重写之前先等 {} 秒让机构的缓存过期（记录 TTL {} 秒）", recordName,
                                 std::chrono::duration_cast<std::chrono::seconds>(quiet).count(), m_configuration.recordTtlSeconds);
                    Core::Timer quietTimer(m_loop);
                    co_await quietTimer.waitFor(quiet);
                }

                // 先查再写：Add 成功但确认阶段超时的上一次，记录其实已经在区里了，
                // 再 Add 一次会被判重复，而重复的答复里没有我们需要的信息
                auto existing = co_await findRecordIds(*target, value);
                if (!existing.has_value())
                {
                    co_return std::unexpected(existing.error());
                }

                if (existing->empty())
                {
                    std::map<std::string, std::string> parameters;
                    parameters["Action"]     = "AddDomainRecord";
                    parameters["DomainName"] = target->zone;
                    parameters["RR"]         = target->rr;
                    parameters["Type"]       = "TXT";
                    parameters["Value"]      = value;
                    parameters["TTL"]        = std::to_string(m_configuration.recordTtlSeconds);
                    if (auto added = co_await call(parameters, "AddDomainRecord"); !added.has_value())
                    {
                        co_return std::unexpected(added.error());
                    }
                }

                // 控制面接受不等于解析服务已经答得出这条：查到为止，查不到就把原因说清
                Core::Timer timer(m_loop);
                const auto  deadline = std::chrono::steady_clock::now() + kVisibilityTimeout;
                while (true)
                {
                    auto visible = co_await findRecordIds(*target, value);
                    if (!visible.has_value())
                    {
                        co_return std::unexpected(visible.error());
                    }
                    if (!visible->empty())
                    {
                        break;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        co_return std::unexpected(std::format("已请求云解析写入 {} 的 TXT，但等了 {} 秒仍查不到这条记录。"
                                                              "查一下这个 AccessKey 有没有 AliyunDNSFullAccess（或等价的只写权限）、"
                                                              "以及 {} 是不是云解析里那条主域名",
                                                              recordName, std::chrono::duration_cast<std::chrono::seconds>(kVisibilityTimeout).count(), target->zone));
                    }
                    co_await timer.waitFor(kVisibilityPollInterval);
                }

                if (m_configuration.publishSettleDelay > std::chrono::milliseconds::zero())
                {
                    co_await timer.waitFor(m_configuration.publishSettleDelay);
                }
                co_return std::expected<void, std::string>{};
            }

            /**
             * @brief 撤掉这条 TXT：按名字与正文反查 RecordId，逐条删
             * @details 查不到就当成功——撤回必须幂等，否则一次没删干净的记录会把下一轮签发挡在
             *          「同一名字两条不同 TXT」上，而机构的答复只会是 DNS 校验失败
             */
            Core::Task<std::expected<void, std::string>> withdraw(std::string recordName, std::string value)
            {
                const std::optional<DnsRecordTarget> target = splitRecordName(recordName, m_configuration.zoneDomainName);
                if (!target.has_value())
                {
                    co_return std::unexpected(zoneSplitFailure(recordName));
                }

                auto found = co_await findRecordIds(*target, value);
                if (!found.has_value())
                {
                    co_return std::unexpected(found.error());
                }

                std::string deletionFailures;
                for (const std::string &recordId: found.value())
                {
                    std::map<std::string, std::string> parameters;
                    parameters["Action"]     = "DeleteDomainRecord";
                    parameters["RecordId"]   = recordId;
                    parameters["DomainName"] = target->zone;
                    if (auto deleted = co_await call(parameters, "DeleteDomainRecord"); !deleted.has_value())
                    {
                        deletionFailures += deletionFailures.empty() ? "" : "; ";
                        deletionFailures += deleted.error();
                    }
                }
                if (!deletionFailures.empty())
                {
                    co_return std::unexpected(deletionFailures);
                }
                if (!found->empty())
                {
                    // 真删掉过才记这一笔：下一次同名重写要按这条记录的 TTL 等机构的缓存过期
                    m_recentWithdrawals[recordName] = std::chrono::steady_clock::now();
                }
                co_return std::expected<void, std::string>{};
            }

        private:
            /**
             * @brief 这个名字距上次撤掉记录，还要等多久才该重写
             * @details 只在进程内记：接连两次签发之间没等满 TTL，机构仍可能答旧值，那一段要靠重跑之间隔开，
             *          库替不了（记到磁盘上就要碰凭据与区文件之外的状态，那是另一件事）
             */
            [[nodiscard]] std::chrono::milliseconds quietPeriodFor(const std::string &recordName) const
            {
                const auto withdrawn = m_recentWithdrawals.find(recordName);
                if (withdrawn == m_recentWithdrawals.end())
                {
                    return std::chrono::milliseconds::zero();
                }
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - withdrawn->second);
                return aliyunRewriteQuietPeriod(elapsed, m_configuration.recordTtlSeconds);
            }

            /**
             * @brief 名字推不出主域名时那条可操作的文案
             * @details 要点名的是**真存在的那几个开关**：配置里是 `acme.dns.domain`，直接构造写入器时是
             *          `AliyunDns01Configuration::zoneDomainName`（探针上对应 `--dns-zone`），
             *          写成一个不存在的配置键会把运维引到一条找不到的路上
             */
            [[nodiscard]] std::string zoneSplitFailure(const std::string_view recordName) const
            {
                return std::format("从记录名 {} 推不出云解析里的主域名：主域名不是「域名最后两段」那种形状时（例如整条主域名本身就是三级域名），"
                                   "把云解析控制台里那条主域名原样交给 acme.dns.domain（直接构造写入器时是 zoneDomainName，探针上是 --dns-zone）",
                                   recordName);
            }

            /**
             * @brief 查这条主域名下与目标值完全一致的 TXT 记录，交回它们的 RecordId
             */
            Core::Task<std::expected<std::vector<std::string>, std::string>> findRecordIds(const DnsRecordTarget &target, const std::string &value)
            {
                std::map<std::string, std::string> parameters;
                parameters["Action"]     = "DescribeDomainRecords";
                parameters["DomainName"] = target.zone;
                parameters["RRKeyWord"]  = target.rr;
                parameters["Type"]       = "TXT";
                parameters["PageNumber"] = "1";
                parameters["PageSize"]   = std::to_string(kQueryPageSize);

                auto replied = co_await call(parameters, "DescribeDomainRecords");
                if (!replied.has_value())
                {
                    co_return std::unexpected(replied.error());
                }

                std::vector<std::string> recordIds;
                const Base::ConfigValue &root = replied.value();
                if (!root.is_object() || !root.contains("DomainRecords"))
                {
                    co_return std::unexpected(std::format("云解析 DescribeDomainRecords 的应答里没有 DomainRecords 成员：{} 这条主域名可能不在这个账号下，"
                                                          "或这个 AccessKey 看不到它",
                                                          target.zone));
                }
                const Base::ConfigValue &container = root.at("DomainRecords");
                if (!container.is_object() || !container.contains("Record") || !container.at("Record").is_array())
                {
                    co_return std::unexpected("云解析 DescribeDomainRecords 的 DomainRecords.Record 不是一数组，形状与预期不符");
                }

                // RRKeyWord 是「包含」匹配，会把 _acme-challenge.sub 那类也带回来；按三段精确条件筛
                for (const Base::ConfigValue &element: container.at("Record"))
                {
                    if (!element.is_object())
                    {
                        continue;
                    }
                    if (readText(element, "RR") != target.rr || readText(element, "Type") != "TXT" || readText(element, "Value") != value)
                    {
                        continue;
                    }
                    if (const std::optional<std::string> recordId = readTextOptional(element, "RecordId"); recordId.has_value())
                    {
                        recordIds.push_back(*recordId);
                    }
                }
                co_return recordIds;
            }

            /**
             * @brief 补上公共参数、签名，发一次 GET，把 JSON 正文交回去
             * @details 失败文案里只带 Action 与错误码/消息，不带完整 URL：URL 里含 Signature，
             *          把它抄进日志等于把一次性的签名留在人人都能看的通道里
             */
            Core::Task<std::expected<Base::ConfigValue, std::string>> call(std::map<std::string, std::string> parameters, std::string_view action)
            {
                if (m_configuration.accessKeyId.empty() || m_configuration.accessKeySecret.empty())
                {
                    co_return std::unexpected("DNS-01 要用阿里云云解析，但凭据是空的：把两条值填进 "
                                              "AliyunDns01Configuration::accessKeyId 与 accessKeySecret"
                                              "（签发探针那两个值从环境变量 ASYN_ACME_DNS_ACCESS_KEY_ID 与 ASYN_ACME_DNS_ACCESS_KEY_SECRET 读）；"
                                              "不要让密钥进配置文件——一份能改域名记录的钥匙进了版本库，等于把域名交出去");
                }

                parameters["Format"]           = "JSON";
                parameters["Version"]          = std::string(kApiVersion);
                parameters["AccessKeyId"]      = m_configuration.accessKeyId;
                parameters["SignatureMethod"]  = "HMAC-SHA1";
                parameters["SignatureVersion"] = "1.0";
                parameters["Timestamp"]        = utcTimestamp();
                parameters["SignatureNonce"]   = makeSignatureNonce();
                // 签名覆盖的是「除 Signature 之外的全部参数」，所以先算进局部量再挂进表里
                const std::string signature = aliyunRpcSignature("GET", parameters, m_configuration.accessKeySecret);
                parameters["Signature"]     = signature;

                std::string query;
                for (const auto &field: parameters)
                {
                    if (!query.empty())
                    {
                        query += '&';
                    }
                    query += aliyunPercentEncode(field.first);
                    query += '=';
                    query += aliyunPercentEncode(field.second);
                }

                HttpClientRequest request;
                auto              sent = co_await HttpClient::send(m_loop, std::string(kEndpointUrl) + "?" + query, request, kRequestTimeout);
                if (!sent.has_value())
                {
                    co_return std::unexpected(std::format("调用阿里云云解析的 {} 失败：{}。dns-01 要求这台机器能出公网 443", action, sent.error()));
                }

                const std::optional<Base::ConfigValue> parsed = Base::parseConfigValue(sent->body);
                if (sent->statusCode != 200)
                {
                    std::string code;
                    std::string message;
                    if (parsed.has_value())
                    {
                        code    = readText(*parsed, "Code");
                        message = readText(*parsed, "Message");
                    }
                    const std::string snippet = sent->body.size() > 200U ? sent->body.substr(0, 200U) : sent->body;
                    co_return std::unexpected(std::format("阿里云云解析拒绝了 {}：HTTP {}{}{}。原文（截断）：{}", action, sent->statusCode, code.empty() ? "" : ", Code=" + code,
                                                          message.empty() ? "" : ", Message=" + message, snippet));
                }
                if (!parsed.has_value())
                {
                    co_return std::unexpected(std::format("阿里云云解析的 {} 回了 HTTP 200 但正文不是合法 JSON，无法继续", action));
                }
                co_return *parsed;
            }

            /**
             * @brief 读一个字符串成员，缺失或类型不对都交回空而不是抛
             */
            [[nodiscard]] static std::optional<std::string> readTextOptional(const Base::ConfigValue &value, const std::string_view memberName)
            {
                if (!value.is_object() || !value.contains(memberName))
                {
                    return std::nullopt;
                }
                return Base::configValueAs<std::string>(value.at(memberName));
            }

            /// 缺失时给空串：调用方只关心「对不对得上」，把缺失与不匹配并成一档
            [[nodiscard]] static std::string readText(const Base::ConfigValue &value, const std::string_view memberName)
            {
                const std::optional<std::string> text = readTextOptional(value, memberName);
                return text.has_value() ? *text : std::string{};
            }

            Core::EventLoop         &m_loop;          ///< 承载出站请求与等待的循环
            AliyunDns01Configuration m_configuration; ///< 凭据与口径
            /// 本进程内真撤过记录的名字与时刻：同名重写之前要按 TTL 等机构的缓存过期
            std::map<std::string, std::chrono::steady_clock::time_point> m_recentWithdrawals;
        };
    } // namespace

    AcmeDns01TxtWriter makeAliyunDns01TxtWriter(Core::EventLoop &loop, AliyunDns01Configuration configuration)
    {
        // 循环对象引用被 shared_ptr 持有的实现捕获：动作对可能被复制进签发协程的帧里，
        // 而管理器活着的时候循环一定活着（见 AcmeCertificateManager 的循环契约）
        auto client = std::make_shared<AlidnsClient>(loop, std::move(configuration));

        AcmeDns01TxtWriter writer;
        // 协程 lambda 的捕获与形参都要按值：帧要在第一次挂起之后仍能读到它们
        writer.publish = [client](std::string fqdn, std::string value) -> Core::Task<std::expected<void, std::string>>
        { co_return co_await client->publish(std::move(fqdn), std::move(value)); };
        writer.withdraw = [client](std::string fqdn, std::string value) -> Core::Task<std::expected<void, std::string>>
        { co_return co_await client->withdraw(std::move(fqdn), std::move(value)); };
        return writer;
    }
} // namespace AsynGyanis::Net
