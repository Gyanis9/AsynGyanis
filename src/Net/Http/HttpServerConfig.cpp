#include "Net/Http/HttpServerConfig.h"

#include "Base/Exception/ConfigValidationException.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    namespace
    {
        /// server 段直接支持的键
        constexpr std::array<std::string_view, 6> kServerKeys{
            "maximum_connections", "maximum_connections_per_ip", "expose_metrics", "limits", "parser_limits", "rate_limit",
        };

        /// limits 子段支持的键
        constexpr std::array<std::string_view, 5> kLimitsKeys{
            "idle_timeout_ms", "read_timeout_ms", "write_timeout_ms", "settings_acknowledgement_timeout_ms",
            "maximum_requests_per_connection",
        };

        /// parser_limits 子段支持的键
        constexpr std::array<std::string_view, 7> kParserLimitsKeys{
            "maximum_uri_length", "maximum_header_field_name_length", "maximum_header_field_value_length",
            "maximum_header_count", "maximum_header_block_length", "maximum_body_size", "maximum_chunk_size_line_length",
        };

        /// rate_limit 子段支持的键
        constexpr std::array<std::string_view, 2> kRateLimitKeys{"requests_per_second", "burst_capacity"};

        /// parser_limits 的结构体字段名与配置键一一对应，指针成员便于逐项读取时不写七遍重复代码
        struct ParserLimitBinding
        {
            std::string_view name;                     ///< 配置键名
            std::size_t HttpParserLimits::*member;     ///< 对应的结构体成员
        };

        constexpr std::array<ParserLimitBinding, kParserLimitsKeys.size()> kParserLimitBindings{{
            {"maximum_uri_length", &HttpParserLimits::maximumUriLength},
            {"maximum_header_field_name_length", &HttpParserLimits::maximumHeaderFieldNameLength},
            {"maximum_header_field_value_length", &HttpParserLimits::maximumHeaderFieldValueLength},
            {"maximum_header_count", &HttpParserLimits::maximumHeaderCount},
            {"maximum_header_block_length", &HttpParserLimits::maximumHeaderBlockLength},
            {"maximum_body_size", &HttpParserLimits::maximumBodySize},
            {"maximum_chunk_size_line_length", &HttpParserLimits::maximumChunkSizeLineLength},
        }};

        /**
         * @brief 把一组可接受的键拼成一句可读的提示
         * @param keys 键表
         * @return std::string 形如 "a、b、c" 的中文枚举
         */
        [[nodiscard]] std::string joinKeys(const std::string_view *keys, const std::size_t keyCount)
        {
            std::string text;
            for (std::size_t index = 0; index < keyCount; ++index)
            {
                if (index != 0)
                {
                    text += "、";
                }
                text += keys[index];
            }
            return text;
        }

        /**
         * @brief 拒绝该层不认得的键
         * @details 未知键一律报错而不是忽略：把 idle_timeout 写成 idle_timeout_ms 时静默忽略等于
         *          「配置没生效却看不出来」，正是最难查的一类问题
         * @param node 待检查的对象节点
         * @param keys 该层支持的键
         * @param pathPrefix 出错时用于拼出完整键路径的前缀
         */
        void rejectUnknownKeys(const Base::ConfigValue &node, const std::string_view *keys, const std::size_t keyCount,
                               const std::string &pathPrefix)
        {
            for (const auto &[memberName, memberValue]: node.items())
            {
                const bool isAccepted = std::any_of(keys, keys + keyCount,
                                                    [&memberName](const std::string_view accepted)
                                                    {
                                                        return accepted == memberName;
                                                    });
                if (!isAccepted)
                {
                    throw Base::ConfigValidationException(pathPrefix + "." + memberName,
                                                          "未知的配置键；本层支持：" + joinKeys(keys, keyCount));
                }
            }
        }

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
         * @brief 把非负整数毫秒换算成时长
         * @param milliseconds 毫秒数（已确保非负）
         * @param key 键路径，用于错误信息
         * @return std::chrono::milliseconds 时长
         * @throws Base::ConfigValidationException 超出毫秒时长的可表示范围
         */
        [[nodiscard]] std::chrono::milliseconds toMilliseconds(const std::uint64_t milliseconds, const std::string &key)
        {
            // 时长的底层是有符号 64 位：超过上限的无符号取值会静默变负，宁可在这里拒绝
            if (milliseconds > static_cast<std::uint64_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max()))
            {
                throw Base::ConfigValidationException(key, "超出可表示的毫秒上限");
            }
            return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(milliseconds));
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
         * @brief 取一个可选的子对象节点
         * @param node 父节点
         * @param key 子键名
         * @param pathPrefix 出错时拼键路径的前缀
         * @return const Base::ConfigValue* 子节点；不存在时为空指针
         * @throws Base::ConfigValidationException 子键存在但不是对象
         */
        [[nodiscard]] const Base::ConfigValue *findOptionalObject(const Base::ConfigValue &node, const std::string_view key,
                                                                 const std::string &pathPrefix)
        {
            if (!node.contains(key))
            {
                return nullptr;
            }
            const Base::ConfigValue &child = node.at(key);
            if (!child.is_object())
            {
                throw Base::ConfigValidationException(pathPrefix + "." + std::string(key), "必须是一个对象");
            }
            return &child;
        }

        /// 读 limits 子段（连接级限额：超时与单连接请求数）
        void readLimitsSection(const Base::ConfigValue &node, HttpServerLimits &limits)
        {
            const std::string sectionPath = std::string(kHttpServerConfigSection) + ".limits";
            rejectUnknownKeys(node, kLimitsKeys.data(), kLimitsKeys.size(), sectionPath);

            // 超时统一按毫秒整数配置；0 表示关闭该项保护（与结构体自身的语义一致）
            if (node.contains("idle_timeout_ms"))
            {
                limits.idleTimeout = toMilliseconds(requireNonNegativeInteger(node.at("idle_timeout_ms"), sectionPath + ".idle_timeout_ms"),
                                                    sectionPath + ".idle_timeout_ms");
            }
            if (node.contains("read_timeout_ms"))
            {
                limits.readTimeout = toMilliseconds(requireNonNegativeInteger(node.at("read_timeout_ms"), sectionPath + ".read_timeout_ms"),
                                                    sectionPath + ".read_timeout_ms");
            }
            if (node.contains("write_timeout_ms"))
            {
                limits.writeTimeout = toMilliseconds(requireNonNegativeInteger(node.at("write_timeout_ms"), sectionPath + ".write_timeout_ms"),
                                                     sectionPath + ".write_timeout_ms");
            }
            if (node.contains("settings_acknowledgement_timeout_ms"))
            {
                limits.settingsAcknowledgementTimeout =
                        toMilliseconds(requireNonNegativeInteger(node.at("settings_acknowledgement_timeout_ms"), sectionPath + ".settings_acknowledgement_timeout_ms"),
                                       sectionPath + ".settings_acknowledgement_timeout_ms");
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
                parserLimits.*binding.member = static_cast<std::size_t>(
                        requireNonNegativeInteger(node.at(binding.name), sectionPath + "." + std::string(binding.name)));
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

        if (section.contains("maximum_connections"))
        {
            configuration.maximumConnections =
                    static_cast<std::size_t>(requireNonNegativeInteger(section.at("maximum_connections"), sectionPath + ".maximum_connections"));
        }
        if (section.contains("maximum_connections_per_ip"))
        {
            configuration.maximumConnectionsPerIp =
                    static_cast<std::size_t>(requireNonNegativeInteger(section.at("maximum_connections_per_ip"), sectionPath + ".maximum_connections_per_ip"));
        }
        if (section.contains("expose_metrics"))
        {
            const Base::ConfigValue &value = section.at("expose_metrics");
            if (!value.is_boolean())
            {
                throw Base::ConfigValidationException(sectionPath + ".expose_metrics", "必须是 true 或 false");
            }
            configuration.exposeMetrics = value.get<bool>();
        }
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

        // 交叉校验：开了限流就必须有能放行至少一个请求的桶，否则令牌桶永远取不出令牌，
        // 表现为「服务一上线就全 429」——配置错误要在这里拦住，而不是等线上发现
        if (configuration.requestsPerSecond > 0.0 && configuration.rateLimitBurstCapacity < 1.0)
        {
            throw Base::ConfigValidationException(std::string(kHttpServerConfigSection) + ".rate_limit.burst_capacity",
                                                  "启用限流时容量必须不小于 1，否则任何请求都放行不了");
        }
        return configuration;
    }

} // namespace AsynGyanis::Net
