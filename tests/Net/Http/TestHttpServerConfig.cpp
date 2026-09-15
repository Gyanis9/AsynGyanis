/**
 * @file TestHttpServerConfig.cpp
 * @brief HttpServerConfig 单元测试：逐键读取、缺省保持默认、未知键与越界值的拒绝面
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Net/Http/HttpServerConfig.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Exception/ConfigValidationException.h"
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpServerLimits.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 把一组 server 段成员包成配置文档的根值
        [[nodiscard]] Base::ConfigValue makeRootDocument(Base::ConfigObject serverMembers)
        {
            Base::ConfigObject root;
            root.emplace(std::string(kHttpServerConfigSection), Base::ConfigValue(std::move(serverMembers)));
            return Base::ConfigValue(std::move(root));
        }

        // 下面几个小工厂只是为了让用例里的树好读：把「这个键该是整数 / 浮点 / 布尔 / 字符串 / 对象」
        // 显式写出来，读用例时不必回头数类型
        [[nodiscard]] Base::ConfigValue integer(const std::int64_t value)
        {
            return Base::ConfigValue(value);
        }

        [[nodiscard]] Base::ConfigValue floating(const double value)
        {
            return Base::ConfigValue(value);
        }

        [[nodiscard]] Base::ConfigValue boolean(const bool value)
        {
            return Base::ConfigValue(value);
        }

        [[nodiscard]] Base::ConfigValue text(std::string value)
        {
            return Base::ConfigValue(std::move(value));
        }

        [[nodiscard]] Base::ConfigValue object(Base::ConfigObject members)
        {
            return Base::ConfigValue(std::move(members));
        }

        // 读配置并断言抛错。GTest 的 EXPECT_THROW 把语句原样放进 try 块，
        // 那里面直接丢弃 [[nodiscard]] 的返回值会被 MSVC 判 C4834，故由这个薄包装显式消费
        void expectConfigurationRejected(const Base::ConfigValue &document)
        {
            static_cast<void>(readHttpServerConfiguration(document));
        }
    } // namespace

    /**
     * @brief 钉住：文档里没有 server 段时，一项都不改（全取结构体默认值）
     */
    TEST(HttpServerConfig, MissingSectionKeepsEveryDefault)
    {
        const Base::ConfigObject root{{"logging", object(Base::ConfigObject{})}};

        const HttpServerConfiguration configuration = readHttpServerConfiguration(Base::ConfigValue(root));

        const HttpServerConfiguration defaults;
        EXPECT_EQ(configuration.limits.idleTimeout, defaults.limits.idleTimeout);
        EXPECT_EQ(configuration.limits.readTimeout, defaults.limits.readTimeout);
        EXPECT_EQ(configuration.limits.maximumRequestsPerConnection, defaults.limits.maximumRequestsPerConnection);
        EXPECT_EQ(configuration.parserLimits.maximumBodySize, defaults.parserLimits.maximumBodySize);
        EXPECT_EQ(configuration.maximumConnections, 0u);
        EXPECT_EQ(configuration.maximumConnectionsPerIp, 0u);
        EXPECT_EQ(configuration.requestsPerSecond, 0.0);
        EXPECT_FALSE(configuration.exposeMetrics);
    }

    /**
     * @brief 钉住：每一个受支持的键都能被读到（漏一个就是「配置写了却不生效」）
     */
    TEST(HttpServerConfig, ReadsEveryDocumentedKey)
    {
        Base::ConfigObject serverMembers;
        serverMembers.emplace("maximum_connections", integer(2048));
        serverMembers.emplace("maximum_connections_per_ip", integer(16));
        serverMembers.emplace("expose_metrics", boolean(true));
        serverMembers.emplace("limits",
                              object(Base::ConfigObject{
                                      {"idle_timeout_ms", integer(30000)},
                                      {"read_timeout_ms", integer(15000)},
                                      {"write_timeout_ms", integer(20000)},
                                      {"settings_acknowledgement_timeout_ms", integer(5000)},
                                      {"maximum_requests_per_connection", integer(100)},
                              }));
        serverMembers.emplace("parser_limits",
                              object(Base::ConfigObject{
                                      {"maximum_uri_length", integer(4096)},
                                      {"maximum_header_field_name_length", integer(128)},
                                      {"maximum_header_field_value_length", integer(2048)},
                                      {"maximum_header_count", integer(64)},
                                      {"maximum_header_block_length", integer(32768)},
                                      {"maximum_body_size", integer(1048576)},
                                      {"maximum_chunk_size_line_length", integer(256)},
                              }));
        serverMembers.emplace("rate_limit",
                              object(Base::ConfigObject{
                                      {"requests_per_second", floating(250.0)},
                                      {"burst_capacity", floating(500.0)},
                              }));

        const HttpServerConfiguration configuration = readHttpServerConfiguration(makeRootDocument(std::move(serverMembers)));

        EXPECT_EQ(configuration.maximumConnections, 2048u);
        EXPECT_EQ(configuration.maximumConnectionsPerIp, 16u);
        EXPECT_TRUE(configuration.exposeMetrics);
        EXPECT_EQ(configuration.limits.idleTimeout, std::chrono::milliseconds(30000));
        EXPECT_EQ(configuration.limits.readTimeout, std::chrono::milliseconds(15000));
        EXPECT_EQ(configuration.limits.writeTimeout, std::chrono::milliseconds(20000));
        EXPECT_EQ(configuration.limits.settingsAcknowledgementTimeout, std::chrono::milliseconds(5000));
        EXPECT_EQ(configuration.limits.maximumRequestsPerConnection, 100u);
        EXPECT_EQ(configuration.parserLimits.maximumUriLength, 4096u);
        EXPECT_EQ(configuration.parserLimits.maximumHeaderFieldNameLength, 128u);
        EXPECT_EQ(configuration.parserLimits.maximumHeaderFieldValueLength, 2048u);
        EXPECT_EQ(configuration.parserLimits.maximumHeaderCount, 64u);
        EXPECT_EQ(configuration.parserLimits.maximumHeaderBlockLength, 32768u);
        EXPECT_EQ(configuration.parserLimits.maximumBodySize, 1048576u);
        EXPECT_EQ(configuration.parserLimits.maximumChunkSizeLineLength, 256u);
        EXPECT_DOUBLE_EQ(configuration.requestsPerSecond, 250.0);
        EXPECT_DOUBLE_EQ(configuration.rateLimitBurstCapacity, 500.0);
    }

    /**
     * @brief 钉住：只写一项时，其余项仍是默认值（升级后新增的键在旧配置上自动取默认）
     */
    TEST(HttpServerConfig, UnspecifiedKeysKeepTheirDefaults)
    {
        const Base::ConfigObject serverMembers{{"maximum_connections", integer(64)}};

        const HttpServerConfiguration configuration = readHttpServerConfiguration(makeRootDocument(serverMembers));
        const HttpServerConfiguration defaults;

        EXPECT_EQ(configuration.maximumConnections, 64u);
        EXPECT_EQ(configuration.limits.readTimeout, defaults.limits.readTimeout);
        EXPECT_EQ(configuration.parserLimits.maximumHeaderCount, defaults.parserLimits.maximumHeaderCount);
    }

    /**
     * @brief 钉住：显式的 0（「关闭该项保护」）与「没写」是两回事，不能被默认值顶掉
     */
    TEST(HttpServerConfig, ExplicitZeroDisablesProtectionInsteadOfFallingBackToDefault)
    {
        const Base::ConfigObject serverMembers{
                {"limits", object(Base::ConfigObject{{"idle_timeout_ms", integer(0)}})},
                {"parser_limits", object(Base::ConfigObject{{"maximum_body_size", integer(0)}})},
        };

        const HttpServerConfiguration configuration = readHttpServerConfiguration(makeRootDocument(serverMembers));

        EXPECT_EQ(configuration.limits.idleTimeout, std::chrono::milliseconds(0));
        EXPECT_EQ(configuration.parserLimits.maximumBodySize, 0u);
    }

    /**
     * @brief 钉住：未知键报错并给出完整键路径——把 _ms 写漏时必须是响亮失败而不是静默忽略
     */
    TEST(HttpServerConfig, RejectsUnknownKeyWithFullKeyPath)
    {
        const Base::ConfigObject serverMembers{
                {"limits", object(Base::ConfigObject{{"idle_timeout", integer(1000)}})},
        };

        try
        {
            static_cast<void>(readHttpServerConfiguration(makeRootDocument(serverMembers)));
            FAIL() << "未知键应当被拒绝";
        } catch (const Base::ConfigValidationException &exception)
        {
            EXPECT_EQ(exception.key(), "server.limits.idle_timeout");
            EXPECT_NE(std::string(exception.what()).find("idle_timeout_ms"), std::string::npos)
                    << "错误信息应把可用的键列出来，否则调用方只能靠猜：「" << exception.what() << "」";
        }
    }

    /**
     * @brief 钉住：类型不符报错（把数字写成字符串是人最容易犯的错）
     */
    TEST(HttpServerConfig, RejectsWrongValueType)
    {
        const Base::ConfigObject serverMembers{
                {"limits", object(Base::ConfigObject{{"read_timeout_ms", text("60000")}})},
        };

        EXPECT_THROW(expectConfigurationRejected(makeRootDocument(serverMembers)), Base::ConfigValidationException);

        const Base::ConfigObject booleanMismatch{{"expose_metrics", text("yes")}};
        EXPECT_THROW(expectConfigurationRejected(makeRootDocument(booleanMismatch)), Base::ConfigValidationException);
    }

    /**
     * @brief 钉住：负数一律拒绝（时间与上限都没有负数的合法解释）
     */
    TEST(HttpServerConfig, RejectsNegativeValues)
    {
        const Base::ConfigObject negativeTimeout{
                {"limits", object(Base::ConfigObject{{"write_timeout_ms", integer(-1)}})},
        };
        EXPECT_THROW(expectConfigurationRejected(makeRootDocument(negativeTimeout)), Base::ConfigValidationException);

        const Base::ConfigObject negativeRate{
                {"rate_limit", object(Base::ConfigObject{{"requests_per_second", floating(-2.0)}})},
        };
        EXPECT_THROW(expectConfigurationRejected(makeRootDocument(negativeRate)), Base::ConfigValidationException);
    }

    /**
     * @brief 钉住：段本身不是对象时报错（写成标量是最常见的结构错误）
     */
    TEST(HttpServerConfig, RejectsNonObjectSection)
    {
        Base::ConfigObject root;
        root.emplace(std::string(kHttpServerConfigSection), integer(42));
        EXPECT_THROW(expectConfigurationRejected(Base::ConfigValue(std::move(root))), Base::ConfigValidationException);

        const Base::ConfigObject scalarLimits{{"limits", integer(1)}};
        EXPECT_THROW(expectConfigurationRejected(makeRootDocument(scalarLimits)), Base::ConfigValidationException);
    }

    /**
     * @brief 钉住：开了限流却给了装不下一个令牌的桶，必须在启动前拦住
     */
    TEST(HttpServerConfig, RejectsRateLimitWithUnusableBurstCapacity)
    {
        const Base::ConfigObject serverMembers{
                {"rate_limit", object(Base::ConfigObject{{"requests_per_second", floating(10.0)}, {"burst_capacity", floating(0.5)}})},
        };

        EXPECT_THROW(expectConfigurationRejected(makeRootDocument(serverMembers)), Base::ConfigValidationException);

        // 不限流时容量取多少都无所谓：没有速率就没有桶
        const Base::ConfigObject unlimitedRequests{
                {"rate_limit", object(Base::ConfigObject{{"requests_per_second", floating(0.0)}, {"burst_capacity", floating(0.0)}})},
        };
        EXPECT_DOUBLE_EQ(readHttpServerConfiguration(makeRootDocument(unlimitedRequests)).requestsPerSecond, 0.0);
    }

    /**
     * @brief 钉住：小数速率可用（打点式的细粒度限流），整数也能当小数读
     */
    TEST(HttpServerConfig, AcceptsFractionalAndIntegralRates)
    {
        const Base::ConfigObject fractional{
                {"rate_limit", object(Base::ConfigObject{{"requests_per_second", floating(2.5)}, {"burst_capacity", floating(3.0)}})},
        };
        const HttpServerConfiguration fractionalConfiguration = readHttpServerConfiguration(makeRootDocument(fractional));
        EXPECT_DOUBLE_EQ(fractionalConfiguration.requestsPerSecond, 2.5);

        const Base::ConfigObject integral{
                {"rate_limit", object(Base::ConfigObject{{"requests_per_second", integer(100)}, {"burst_capacity", integer(200)}})},
        };
        const HttpServerConfiguration integralConfiguration = readHttpServerConfiguration(makeRootDocument(integral));
        EXPECT_DOUBLE_EQ(integralConfiguration.requestsPerSecond, 100.0);
        EXPECT_DOUBLE_EQ(integralConfiguration.rateLimitBurstCapacity, 200.0);
    }

} // namespace AsynGyanis::Net
