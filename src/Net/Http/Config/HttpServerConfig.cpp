#include "Net/Http/Config/HttpServerConfig.h"

#include "Net/Http/Config/HttpConfigValidation.h"
#include "Net/Http/Config/HttpConfigDuration.h"
#include "Base/Exception/ConfigValidationException.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/Message/HttpHeaderRules.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    namespace
    {
        using HttpConfig::findOptionalObject;
        using HttpConfig::joinKeys;
        using HttpConfig::rejectUnknownKeys;

        using HttpConfig::toMilliseconds;

        /// server 段直接支持的键
        constexpr std::array<std::string_view, 13> kServerKeys{
                "maximum_connections",
                "maximum_connections_per_ip",
                "memory_budget_bytes",
                "expose_metrics",
                "ops_bearer_token",
                "metrics_port",
                "metrics_address",
                "access_log",
                "handler_timeout_ms",
                "limits",
                "parser_limits",
                "rate_limit",
                "tls",
        };

        /// tls 子段支持的键：逐项对应 Core::TlsPolicy 的旋钮，外加「要不要客户端证书」这一格
        constexpr std::array<std::string_view, 14> kTlsKeys{
                "minimum_protocol_version",
                "maximum_protocol_version",
                "cipher_list",
                "tls13_cipher_suites",
                "supported_groups",
                "security_level",
                "certificate_authority_file",
                "certificate_authority_path",
                "verify_depth",
                "revocation_list_file",
                "revocation_covers_whole_chain",
                "session_tickets",
                "allowed_peer_sha256_fingerprints",
                "require_client_certificates",
        };

        /// limits 子段支持的键
        constexpr std::array<std::string_view, 6> kLimitsKeys{
                "idle_timeout_ms", "read_timeout_ms", "write_timeout_ms", "settings_acknowledgement_timeout_ms", "web_socket_pong_timeout_ms", "maximum_requests_per_connection",
        };

        /// parser_limits 子段支持的键
        constexpr std::array<std::string_view, 8> kParserLimitsKeys{
                "maximum_uri_length", "maximum_header_field_name_length", "maximum_header_field_value_length", "maximum_header_count", "maximum_header_block_length",
                "maximum_body_size",  "maximum_chunk_size_line_length",   "maximum_websocket_message_size",
        };

        /// rate_limit 子段支持的键
        constexpr std::array<std::string_view, 2> kRateLimitKeys{"requests_per_second", "burst_capacity"};

        /// parser_limits 的结构体字段名与配置键一一对应，指针成员便于逐项读取时不写八遍重复代码
        struct ParserLimitBinding
        {
            std::string_view name;                 ///< 配置键名
            std::size_t HttpParserLimits::*member; ///< 对应的结构体成员
        };

        constexpr std::array<ParserLimitBinding, kParserLimitsKeys.size()> kParserLimitBindings{{
                {"maximum_uri_length", &HttpParserLimits::maximumUriLength},
                {"maximum_header_field_name_length", &HttpParserLimits::maximumHeaderFieldNameLength},
                {"maximum_header_field_value_length", &HttpParserLimits::maximumHeaderFieldValueLength},
                {"maximum_header_count", &HttpParserLimits::maximumHeaderCount},
                {"maximum_header_block_length", &HttpParserLimits::maximumHeaderBlockLength},
                {"maximum_body_size", &HttpParserLimits::maximumBodySize},
                {"maximum_chunk_size_line_length", &HttpParserLimits::maximumChunkSizeLineLength},
                {"maximum_websocket_message_size", &HttpParserLimits::maximumWebsocketMessageSize},
        }};
        /**
         * @brief 从值里取一个非负整数
         * @param value 配置值
         * @param key 键路径，用于错误信息
         * @return std::uint64_t 取值
         * @throws Base::ConfigValidationException 类型不是整数或是负数
         */
        [[nodiscard]] std::uint64_t requireNonNegativeInteger(const Base::ConfigValue &value, const std::string &key)
        {
            if (!value.is_number_integer() && !value.is_number_unsigned())
            {
                throw Base::ConfigValidationException(key, "必须是整数（不能写成字符串或小数）");
            }

            // 原生解析把非负整数放进 number_unsigned、负整数放进 number_integer，
            // 两种都要认，且按各自的实际类型取值，不做跨类型转换
            if (value.is_number_integer())
            {
                const std::int64_t signedValue = value.get<std::int64_t>();
                if (signedValue < 0)
                {
                    throw Base::ConfigValidationException(key, "不能是负数");
                }
                return static_cast<std::uint64_t>(signedValue);
            }
            return value.get<std::uint64_t>();
        }

        /**
         * @brief 读一个只认真布尔的开关键：没写这个键就保持调用方传进来的默认值
         * @details 只认 true/false。`on`/`yes`/`1`/`enabled` 在 YAML 里各有自己的解释，猜中一侧等于
         *          「配了关而实际还开着」——开关读错要比读漏更响亮地失败
         * @param section 本层的对象节点
         * @param key 键名
         * @param keyPath 该键的完整路径，用于错误信息
         * @param destination 取值落点
         * @throws Base::ConfigValidationException 值不是布尔
         */
        void readBooleanSwitch(const Base::ConfigValue &section, const std::string &key, const std::string &keyPath, bool &destination)
        {
            if (!section.contains(key))
            {
                return;
            }
            const Base::ConfigValue &value = section.at(key);
            if (!value.is_boolean())
            {
                throw Base::ConfigValidationException(keyPath, "必须是 true 或 false");
            }
            destination = value.get<bool>();
        }
        /**
         * @brief 从值里取一个非负数值（整数或小数都接受）
         * @param value 配置值
         * @param key 键路径，用于错误信息
         * @return double 取值
         * @throws Base::ConfigValidationException 类型不是数值或是负数
         */
        [[nodiscard]] double requireNonNegativeNumber(const Base::ConfigValue &value, const std::string &key)
        {
            if (!value.is_number())
            {
                throw Base::ConfigValidationException(key, "必须是数值");
            }

            const double number = value.is_number_float() ? value.get<double>() : static_cast<double>(requireNonNegativeInteger(value, key));
            // NaN 与无穷大连「是负数」都比不出来：放行会让限流配置静默失效
            if (!std::isfinite(number))
            {
                throw Base::ConfigValidationException(key, "必须是有限数值（NaN 与无穷大没有合法解释）");
            }
            if (number < 0.0)
            {
                throw Base::ConfigValidationException(key, "不能是负数");
            }
            return number;
        }
        /**
         * @brief 把协议版本档位文本解成策略里的枚举
         * @details 只认这两档：更低的 TLS 1.0/1.1 由 RFC 8996 列为废弃，本框架不接受、也不给配置项——
         *          留一个「能把它调回去」的口子等于把服务端重新暴露给 BEAST 一类的已知攻击面。
         *          比较按 ASCII 折大小写（`equalsIgnoringCase`），`tls1.3` 与 `TLS1.3` 同解
         * @param value 配置值
         * @param keyPath 键路径，用于错误信息
         * @return Core::TlsPolicy::ProtocolVersion 解出的档位
         * @throws Base::ConfigValidationException 不是字符串，或取值不在这两档之内
         */
        [[nodiscard]] Core::TlsPolicy::ProtocolVersion readProtocolVersion(const Base::ConfigValue &value, const std::string &keyPath)
        {
            if (!value.is_string())
            {
                throw Base::ConfigValidationException(keyPath, "必须是字符串（TLS1.2 或 TLS1.3）");
            }
            const std::string text = value.get<std::string>();
            if (equalsIgnoringCase(text, "TLS1.2"))
            {
                return Core::TlsPolicy::ProtocolVersion::Tls1_2;
            }
            if (equalsIgnoringCase(text, "TLS1.3"))
            {
                return Core::TlsPolicy::ProtocolVersion::Tls1_3;
            }
            throw Base::ConfigValidationException(keyPath, "只支持 TLS1.2 与 TLS1.3（更低的两档由 RFC 8996 列为废弃，这里既不接受也不给取值），当前是「" + text + "」");
        }

        /**
         * @brief 取一条可选的字符串字段：没给就留空，而策略里空串的含义正是「这一格不动」
         * @param node 本层的对象节点
         * @param key 键名
         * @param keyPath 该键的完整路径，用于错误信息
         * @param destination 取值落点
         * @throws Base::ConfigValidationException 值不是字符串
         */
        void readOptionalStringInto(const Base::ConfigValue &node, const std::string &key, const std::string &keyPath, std::string &destination)
        {
            if (!node.contains(key))
            {
                return;
            }
            const Base::ConfigValue &value = node.at(key);
            if (!value.is_string())
            {
                throw Base::ConfigValidationException(keyPath, "必须是字符串（一条路径或一段 OpenSSL 语法的原文）");
            }
            destination = value.get<std::string>();
        }

        /**
         * @brief 读 server.tls 段：只做文本到 `Core::TlsPolicy` 的映射，不新造中间类型
         * @details 套件串、TLS 1.3 套件与曲线名刻意不预先校验：什么算合法由 OpenSSL 说了算（它随发行版
         *          的编译配置变化），自己解析一遍会把它支持的写法判成非法；被拒时启动抛出的消息带着那份
         *          原文与出错的那一项，比这里再猜一遍准。指纹名单同理——那条判据住在 `TlsContext`
         *          （64 位小写十六进制），两处各写一份迟早会漂
         * @param node tls 段的对象节点
         * @param configuration 落点：@c tlsPolicy 与 @c requireClientCertificates
         * @param sectionPath 上级键路径（"server"），用于拼出完整键名
         * @throws Base::ConfigValidationException 出现未知键、类型不符或取值越界
         */
        void readTlsSection(const Base::ConfigValue &node, HttpServerConfiguration &configuration, const std::string &sectionPath)
        {
            const std::string tlsPath = sectionPath + ".tls";
            rejectUnknownKeys(node, kTlsKeys.data(), kTlsKeys.size(), tlsPath);
            Core::TlsPolicy &policy = configuration.tlsPolicy;

            if (node.contains("minimum_protocol_version"))
            {
                policy.minimumProtocolVersion = readProtocolVersion(node.at("minimum_protocol_version"), tlsPath + ".minimum_protocol_version");
            }
            if (node.contains("maximum_protocol_version"))
            {
                policy.maximumProtocolVersion = readProtocolVersion(node.at("maximum_protocol_version"), tlsPath + ".maximum_protocol_version");
            }

            readOptionalStringInto(node, "cipher_list", tlsPath + ".cipher_list", policy.cipherList);
            readOptionalStringInto(node, "tls13_cipher_suites", tlsPath + ".tls13_cipher_suites", policy.tls13CipherSuites);
            readOptionalStringInto(node, "supported_groups", tlsPath + ".supported_groups", policy.supportedGroups);
            readOptionalStringInto(node, "certificate_authority_file", tlsPath + ".certificate_authority_file", policy.certificateAuthorityFile);
            readOptionalStringInto(node, "certificate_authority_path", tlsPath + ".certificate_authority_path", policy.certificateAuthorityPath);
            readOptionalStringInto(node, "revocation_list_file", tlsPath + ".revocation_list_file", policy.revocationListFile);

            if (node.contains("security_level"))
            {
                // OpenSSL 的等级只有 0..5 这六档有效（更高按 5 处理），负数是「把整套检查关掉」，
                // 那不是任何部署会想要的写法，因此两侧都拒
                const std::int64_t securityLevel = requireNonNegativeInteger(node.at("security_level"), tlsPath + ".security_level");
                if (securityLevel > 5)
                {
                    throw Base::ConfigValidationException(
                            tlsPath + ".security_level",
                            std::format("必须在 0-5 之间（0 = 只做最小防护，2 = 默认档：拒 1024 位以下的 RSA/DH 与 SHA-1 签名），当前是 {}", securityLevel));
                }
                policy.securityLevel = static_cast<int>(securityLevel);
            }
            if (node.contains("verify_depth"))
            {
                const std::int64_t depth = requireNonNegativeInteger(node.at("verify_depth"), tlsPath + ".verify_depth");
                if (depth < 1)
                {
                    throw Base::ConfigValidationException(tlsPath + ".verify_depth", "必须不小于 1：0 表示连对端那一张证书都不允许验，等于每一条握手必失败");
                }
                policy.verifyDepth = static_cast<int>(depth);
            }
            readBooleanSwitch(node, "revocation_covers_whole_chain", tlsPath + ".revocation_covers_whole_chain", policy.revocationCoversWholeChain);
            readBooleanSwitch(node, "session_tickets", tlsPath + ".session_tickets", policy.areSessionTicketsEnabled);
            readBooleanSwitch(node, "require_client_certificates", tlsPath + ".require_client_certificates", configuration.requireClientCertificates);

            if (node.contains("allowed_peer_sha256_fingerprints"))
            {
                const Base::ConfigValue &value = node.at("allowed_peer_sha256_fingerprints");
                if (!value.is_array())
                {
                    throw Base::ConfigValidationException(tlsPath + ".allowed_peer_sha256_fingerprints", "必须是列表，每一项是一条指纹（单条也要写成列表）");
                }
                policy.allowedPeerSha256Fingerprints.clear();
                for (const Base::ConfigValue &item: value)
                {
                    if (!item.is_string())
                    {
                        throw Base::ConfigValidationException(tlsPath + ".allowed_peer_sha256_fingerprints",
                                                              "每一项都必须是字符串指纹（`openssl x509 -noout -fingerprint -sha256` 取出的那 64 位十六进制）");
                    }
                    policy.allowedPeerSha256Fingerprints.push_back(item.get<std::string>());
                }
            }
        }

        /// 读 limits 子段（连接级限额：超时与单连接请求数）
        void readLimitsSection(const Base::ConfigValue &node, HttpServerLimits &limits)
        {
            const std::string sectionPath = std::string(kHttpServerConfigSection) + ".limits";
            rejectUnknownKeys(node, kLimitsKeys.data(), kLimitsKeys.size(), sectionPath);

            // 超时统一按毫秒整数配置；0 表示关闭该项保护（与结构体自身的语义一致）
            if (node.contains("idle_timeout_ms"))
            {
                limits.idleTimeout = toMilliseconds(requireNonNegativeInteger(node.at("idle_timeout_ms"), sectionPath + ".idle_timeout_ms"), sectionPath + ".idle_timeout_ms");
            }
            if (node.contains("read_timeout_ms"))
            {
                limits.readTimeout = toMilliseconds(requireNonNegativeInteger(node.at("read_timeout_ms"), sectionPath + ".read_timeout_ms"), sectionPath + ".read_timeout_ms");
            }
            if (node.contains("write_timeout_ms"))
            {
                limits.writeTimeout = toMilliseconds(requireNonNegativeInteger(node.at("write_timeout_ms"), sectionPath + ".write_timeout_ms"), sectionPath + ".write_timeout_ms");
            }
            if (node.contains("settings_acknowledgement_timeout_ms"))
            {
                limits.settingsAcknowledgementTimeout =
                        toMilliseconds(requireNonNegativeInteger(node.at("settings_acknowledgement_timeout_ms"), sectionPath + ".settings_acknowledgement_timeout_ms"),
                                       sectionPath + ".settings_acknowledgement_timeout_ms");
            }
            if (node.contains("web_socket_pong_timeout_ms"))
            {
                limits.webSocketPongTimeout = toMilliseconds(requireNonNegativeInteger(node.at("web_socket_pong_timeout_ms"), sectionPath + ".web_socket_pong_timeout_ms"),
                                                             sectionPath + ".web_socket_pong_timeout_ms");
            }
            if (node.contains("maximum_requests_per_connection"))
            {
                limits.maximumRequestsPerConnection =
                        static_cast<std::size_t>(requireNonNegativeInteger(node.at("maximum_requests_per_connection"), sectionPath + ".maximum_requests_per_connection"));
            }
        }

        /// 读 parser_limits 子段（单条报文的内存上限）
        void readParserLimitsSection(const Base::ConfigValue &node, HttpParserLimits &parserLimits)
        {
            const std::string sectionPath = std::string(kHttpServerConfigSection) + ".parser_limits";
            rejectUnknownKeys(node, kParserLimitsKeys.data(), kParserLimitsKeys.size(), sectionPath);

            for (const ParserLimitBinding &binding: kParserLimitBindings)
            {
                if (!node.contains(binding.name))
                {
                    continue;
                }
                parserLimits.*binding.member = static_cast<std::size_t>(requireNonNegativeInteger(node.at(binding.name), sectionPath + "." + std::string(binding.name)));
            }
        }

        /// 读 rate_limit 子段（令牌桶速率与容量）
        void readRateLimitSection(const Base::ConfigValue &node, double &requestsPerSecond, double &burstCapacity)
        {
            const std::string sectionPath = std::string(kHttpServerConfigSection) + ".rate_limit";
            rejectUnknownKeys(node, kRateLimitKeys.data(), kRateLimitKeys.size(), sectionPath);

            if (node.contains("requests_per_second"))
            {
                requestsPerSecond = requireNonNegativeNumber(node.at("requests_per_second"), sectionPath + ".requests_per_second");
            }
            if (node.contains("burst_capacity"))
            {
                burstCapacity = requireNonNegativeNumber(node.at("burst_capacity"), sectionPath + ".burst_capacity");
            }
        }

        /**
         * @brief 读 server 段的标量键（连接数、内存预算、运维面那几格、访问日志与处理时限）
         * @details 与子段读取分开是因为职责不同，不是因为长度：这一层每项都是「一个键 → 一个字段」，
         *          而 limits/parser_limits/rate_limit/tls 各自还要往下判一层。分开放，新加一格标量键
         *          时不必在最外层函数里把已有那十几个键重看一遍
         * @param section server 段的对象节点（未知键已由调用处拒过）
         * @param configuration 取值落点
         * @param sectionPath 上级键路径（"server"），用于拼出完整键名
         * @throws Base::ConfigValidationException 类型不符或取值越界
         */
        void readServerScalarKeys(const Base::ConfigValue &section, HttpServerConfiguration &configuration, const std::string &sectionPath)
        {
            if (section.contains("maximum_connections"))
            {
                configuration.maximumConnections = static_cast<std::size_t>(requireNonNegativeInteger(section.at("maximum_connections"), sectionPath + ".maximum_connections"));
            }
            if (section.contains("maximum_connections_per_ip"))
            {
                configuration.maximumConnectionsPerIp =
                        static_cast<std::size_t>(requireNonNegativeInteger(section.at("maximum_connections_per_ip"), sectionPath + ".maximum_connections_per_ip"));
            }
            if (section.contains("memory_budget_bytes"))
            {
                configuration.memoryBudgetBytes = static_cast<std::size_t>(requireNonNegativeInteger(section.at("memory_budget_bytes"), sectionPath + ".memory_budget_bytes"));
            }
            readBooleanSwitch(section, "expose_metrics", sectionPath + ".expose_metrics", configuration.exposeMetrics);
            if (section.contains("ops_bearer_token"))
            {
                const Base::ConfigValue &value = section.at("ops_bearer_token");
                if (!value.is_string())
                {
                    throw Base::ConfigValidationException(sectionPath + ".ops_bearer_token", "必须是字符串；不想要鉴权就把这一项整个删掉，写 null 或空串都不是「删掉」");
                }
                configuration.opsBearerToken = value.get<std::string>();
                // 空串是最坏的一种写法：中间件会因「没令牌」而抛，或者被当成不鉴权——两种都比「你没配」更难读。
                // 要取消鉴权就不写这一项，要写就得写一个真的值
                if (configuration.opsBearerToken.empty())
                {
                    throw Base::ConfigValidationException(sectionPath + ".ops_bearer_token", "不能是空串：空串等于挂一道永远放行的闸。不需要鉴权就删掉这一项");
                }
            }
            if (section.contains("metrics_port"))
            {
                const std::int64_t portValue = requireNonNegativeInteger(section.at("metrics_port"), sectionPath + ".metrics_port");
                // 端口要装得进 uint16_t：静默取模会让进程去听一个谁也没配的端口，症状是「指标抓不到」
                // 而配置文件看着完全正确——这类数一旦越界只能拒，不能折
                if (portValue > std::numeric_limits<std::uint16_t>::max())
                {
                    throw Base::ConfigValidationException(sectionPath + ".metrics_port", std::format("必须在 0-{} 之间（0 = 运维端点留在业务口上），当前是 {}",
                                                                                                     std::numeric_limits<std::uint16_t>::max(), portValue));
                }
                configuration.metricsPort = static_cast<std::uint16_t>(portValue);
            }
            if (section.contains("metrics_address"))
            {
                const Base::ConfigValue &value = section.at("metrics_address");
                if (!value.is_string())
                {
                    throw Base::ConfigValidationException(sectionPath + ".metrics_address", "必须是 IP 文本字符串（如 \"127.0.0.1\" 或 \"::1\"）");
                }
                configuration.metricsAddress = value.get<std::string>();
                // 地址在读配置时就判能不能解析：留到 bind 那一刻失败，报出来的是平台错误码，
                // 读的人得先懂 EADDRNOTAVAIL 才知道是「metrics_address 写错了」
                if (!Core::InetAddress::parseLiteral(configuration.metricsAddress, 0).has_value())
                {
                    throw Base::ConfigValidationException(
                            sectionPath + ".metrics_address",
                            std::format("不是可解析的 IP 字面量：「{}」。管理口要只听本机写 127.0.0.1（IPv6 用 ::1），要对外监听才写 0.0.0.0", configuration.metricsAddress));
                }
            }
            readBooleanSwitch(section, "access_log", sectionPath + ".access_log", configuration.accessLog);
            // 处理时限与连接级那几项同一口径：0 = 关闭该项保护，非 0 要能与当前时刻相加（同一把尺见 toMilliseconds）
            if (section.contains("handler_timeout_ms"))
            {
                configuration.handlerTimeout =
                        toMilliseconds(requireNonNegativeInteger(section.at("handler_timeout_ms"), sectionPath + ".handler_timeout_ms"), sectionPath + ".handler_timeout_ms");
            }
        }

        /**
         * @brief 交叉校验：「配了这一项、而它的前提没配」的那几类状态，在读配置那一刻拦住
         * @details 静默按其中一半生效时，配置文件每一行看着都对，而线上表现与出错的位置隔得很远：
         *          容量小于 1 的桶让服务一上线就全 429；没开运维端点的令牌保护不到任何东西；
         *          单独一个管理口端口号没有端点可听；要求客户端证书却没有信任锚时每条握手都失败。
         *          这些都只能在装配前说，留到运行期就只剩「配了却没生效」这一种读法
         * @param configuration 已读出的整份配置
         * @throws Base::ConfigValidationException 任一条不成立，键路径点名到那一格
         */
        void validateCrossKeyConstraints(const HttpServerConfiguration &configuration)
        {
            // 开了限流就必须有能放行至少一个请求的桶，否则令牌桶永远取不出令牌
            if (configuration.requestsPerSecond > 0.0 && configuration.rateLimitBurstCapacity < 1.0)
            {
                throw Base::ConfigValidationException(std::string(kHttpServerConfigSection) + ".rate_limit.burst_capacity", "启用限流时容量必须不小于 1，否则任何请求都放行不了");
            }
            // 配了运维端点令牌却没开 expose_metrics，闸门就保护不到任何东西——看起来像「已配鉴权」
            // 而实际无人在用它，是最难发现的一类死配置
            if (!configuration.opsBearerToken.empty() && !configuration.exposeMetrics)
            {
                throw Base::ConfigValidationException(std::string(kHttpServerConfigSection) + ".ops_bearer_token",
                                                      "配了令牌但没有 expose_metrics：闸门保护不到任何端点。要么两处都开，要么把令牌这一项删掉");
            }
            // 同理：单独一个管理口端口号没有任何端点可听，或者只改了监听地址却没开管理口——
            // 这两种都让「来源收口」这件事看起来做了而实际没做
            if (configuration.metricsPort != 0 && !configuration.exposeMetrics)
            {
                throw Base::ConfigValidationException(std::string(kHttpServerConfigSection) + ".metrics_port",
                                                      "配了管理口端口却没有 expose_metrics：这个口上不会有任何端点。要么两处都开，要么把 metrics_port 设回 0");
            }
            if (configuration.metricsPort == 0 && configuration.metricsAddress != "127.0.0.1")
            {
                throw Base::ConfigValidationException(std::string(kHttpServerConfigSection) + ".metrics_address",
                                                      std::format("metrics_port 是 0（端点留在业务口上），此时 metrics_address=「{}」不会被任何人使用。"
                                                                  "要给运维端点单独收口就同时设 metrics_port；只想公开到所有网卡就别写这一项",
                                                                  configuration.metricsAddress));
            }
            // 同一类状态落到 TLS 上更狠：要求客户端证书却没有信任锚，在握手里的形态是每条连接都失败
            // （HTTPS 与 QUIC 两侧的构造期判据都是拒），因此排在这里说，而不是留一条永远握不成的监听器
            if (configuration.requireClientCertificates && configuration.tlsPolicy.certificateAuthorityFile.empty() && configuration.tlsPolicy.certificateAuthorityPath.empty())
            {
                throw Base::ConfigValidationException(std::string(kHttpServerConfigSection) + ".tls.require_client_certificates",
                                                      "开了双向 TLS 却没给 certificate_authority_file 或 certificate_authority_path：没有信任锚就没有任何证书能过校验，"
                                                      "每条握手都会失败。要么补上 CA，要么把这一项设回 false");
            }
            // 同理：「整链都查」说的是查哪些证书，一张列表都没有时这句话没有对象可查
            if (configuration.tlsPolicy.revocationCoversWholeChain && configuration.tlsPolicy.revocationListFile.empty())
            {
                throw Base::ConfigValidationException(std::string(kHttpServerConfigSection) + ".tls.revocation_covers_whole_chain",
                                                      "配了整链吊销检查却没给 revocation_list_file：没有任何列表可查，这一项不会被任何人使用。要么给出 CRL 文件，要么设回 false");
            }
            // 名单是「链验通之上再点名」：不要求客户端证书时服务端根本不会向对端要证书，这份名单一次也
            // 不会被查到。`TlsContext` 对「没开校验就设名单」是直接拒，配置这一层同一条判据提前说
            if (!configuration.requireClientCertificates && !configuration.tlsPolicy.allowedPeerSha256Fingerprints.empty())
            {
                throw Base::ConfigValidationException(std::string(kHttpServerConfigSection) + ".tls.allowed_peer_sha256_fingerprints",
                                                      "给了对端指纹名单却没开 require_client_certificates：服务端不会向对端要证书，名单一次也不会被查。"
                                                      "要么两处都开，要么删掉 allowed_peer_sha256_fingerprints");
            }
        }
    } // namespace

    HttpServerConfiguration readHttpServerConfiguration(const Base::ConfigValue &configurationRoot)
    {
        HttpServerConfiguration configuration;

        // 整段缺失即「全用默认值」：不必为了不改任何限额而写一个空的 server 段
        if (!configurationRoot.contains(kHttpServerConfigSection))
        {
            return configuration;
        }
        const Base::ConfigValue &section = configurationRoot.at(kHttpServerConfigSection);
        if (section.is_null())
        {
            return configuration;
        }
        if (!section.is_object())
        {
            throw Base::ConfigValidationException(std::string(kHttpServerConfigSection), "必须是一个对象");
        }

        const std::string sectionPath(kHttpServerConfigSection);
        rejectUnknownKeys(section, kServerKeys.data(), kServerKeys.size(), sectionPath);

        readServerScalarKeys(section, configuration, sectionPath);

        // 有意不暴露空闲清扫节拍：它是超时误差的唯一来源（最坏误差 = 节拍 + 各连接自己的超时），
        // 手调它只会把超时语义调坏；需要更细的节拍应当在代码里改而不是配置里拧

        if (const Base::ConfigValue *limitsNode = findOptionalObject(section, "limits", sectionPath))
        {
            readLimitsSection(*limitsNode, configuration.limits);
        }
        if (const Base::ConfigValue *parserNode = findOptionalObject(section, "parser_limits", sectionPath))
        {
            readParserLimitsSection(*parserNode, configuration.parserLimits);
        }
        if (const Base::ConfigValue *rateNode = findOptionalObject(section, "rate_limit", sectionPath))
        {
            readRateLimitSection(*rateNode, configuration.requestsPerSecond, configuration.rateLimitBurstCapacity);
        }
        if (const Base::ConfigValue *tlsNode = findOptionalObject(section, "tls", sectionPath))
        {
            readTlsSection(*tlsNode, configuration, sectionPath);
        }

        // 交叉校验（限流桶、运维端点三件套、TLS 的信任锚与吊销名单要配套）都在那一个函数里
        validateCrossKeyConstraints(configuration);
        return configuration;
    }

} // namespace AsynGyanis::Net
