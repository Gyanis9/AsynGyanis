// /debug/loops 的应答渲染（纯函数，逐字段钉）与回环上的一次真实抓取

#include "Net/Http/HttpMetricsEndpoint.h"
#include "Net/Http/HttpServer.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Core/EventLoop/EventLoop.h"
#include "HttpTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;

        using Base::ConfigValue;
        using Base::parseConfigValue;

        /// 抓取观测端点时的等待上限
        constexpr std::chrono::milliseconds kEndpointTimeout{2000};

        /// 渲染用例的基准时刻：所有「已持续多久」都按这一时刻算，于是正文可逐字段断言
        const auto kRenderMoment = std::chrono::steady_clock::time_point{std::chrono::seconds{1000}};

        /**
         * @brief 造一行循环快照
         * @param serialNumber 槽位号
         * @param phase 当前相
         * @param isRunning 是否还在跑
         * @param phaseAge 这一相已持续多久（从 kRenderMoment 往回推）
         * @return Core::ObservedEventLoop 可直接喂给渲染函数的一行
         */
        [[nodiscard]] Core::ObservedEventLoop makeLoopRow(const std::uint64_t serialNumber,
                                                          const Core::LoopPhase phase,
                                                          const bool isRunning,
                                                          const std::chrono::milliseconds phaseAge)
        {
            return Core::ObservedEventLoop{
                    .serialNumber = serialNumber,
                    .snapshot     = Core::EventLoopSnapshot{
                            .ownerThread              = std::thread::id{},
                            .isRunning                = isRunning,
                            .phase                    = phase,
                            .phaseStartedAt           = kRenderMoment - phaseAge,
                            .completedWorkingSegments = 42,
                            .slowestWorkingSegment    = std::chrono::microseconds{7},
                            .remotePendingCount       = 3,
                    },
            };
        }

        /// 把渲染结果读回 JSON 树；渲染坏了直接让用例带着正文失败
        [[nodiscard]] ConfigValue parseBody(const std::string &text)
        {
            const auto parsed = parseConfigValue(text);
            EXPECT_TRUE(parsed.has_value()) << "渲染结果不是合法 JSON：" << text;
            return parsed.value_or(ConfigValue{});
        }
    } // namespace

    /**
     * @brief 钉住：只有「还在跑 + 正在干活 + 超过阈值」三者同时成立才算停顿
     * @details 一行一种反例：等事件的循环再久也只是空闲，停下的循环相位时刻不再更新，
     *          干活没超阈值的循环是正常服务。三条反例与一条正例放在一起，去掉任一条件都会红
     */
    TEST(LoopDiagnosticsEndpoint, JudgesStallOnlyForALiveWorkingLoopBeyondThreshold)
    {
        const std::vector<Core::ObservedEventLoop> rows{
                makeLoopRow(1, Core::LoopPhase::Working, true, std::chrono::milliseconds{500}),   // 真停顿
                makeLoopRow(2, Core::LoopPhase::Working, true, std::chrono::milliseconds{5}),     // 干活但没超阈值
                makeLoopRow(3, Core::LoopPhase::WaitingForEvents, true, std::chrono::seconds{60}),// 空闲
                makeLoopRow(4, Core::LoopPhase::Working, false, std::chrono::seconds{60}),        // 已停止
        };

        const ConfigValue parsed = parseBody(formatLoopDiagnosticsJson(rows, 0, kRenderMoment));
        EXPECT_EQ(parsed.at("stallThresholdMicroseconds"), 100000);
        EXPECT_EQ(parsed.at("unregisteredLoopCount"), 0);

        const ConfigValue &loops = parsed.at("loops");
        ASSERT_EQ(loops.size(), 4U);
        EXPECT_TRUE(loops.at(0).at("stalled").get<bool>());
        EXPECT_FALSE(loops.at(1).at("stalled").get<bool>());
        EXPECT_FALSE(loops.at(2).at("stalled").get<bool>());
        EXPECT_FALSE(loops.at(3).at("stalled").get<bool>()) << "已停下的循环不能算停顿：它的相位时刻不再更新，读出来会是一个很大的假数";

        // 字段名与单位是对外契约的一部分，改任何一个都要有用例挡着
        EXPECT_EQ(loops.at(0).at("serial"), 1);
        EXPECT_EQ(loops.at(0).at("phase"), "working");
        EXPECT_EQ(loops.at(2).at("phase"), "waiting_for_events");
        EXPECT_EQ(loops.at(3).at("phase"), "working");
        EXPECT_TRUE(loops.at(0).at("running").get<bool>());
        EXPECT_FALSE(loops.at(3).at("running").get<bool>());
        EXPECT_EQ(loops.at(0).at("phaseMicroseconds"), 500000);
        EXPECT_EQ(loops.at(0).at("completedWorkingSegments"), 42);
        EXPECT_EQ(loops.at(0).at("slowestWorkingSegmentMicroseconds"), 7);
        EXPECT_EQ(loops.at(0).at("remotePendingCount"), 3);
        EXPECT_TRUE(loops.at(0).at("thread").is_string());
        EXPECT_EQ(loops.at(0).at("thread").get<std::string>().size(), 16U) << "线程标识折成定宽十六进制指纹，行与行之间要能分开";
    }

    /**
     * @brief 钉住：一条循环都没有时也是合法 JSON，且未登记差额照样报出
     */
    TEST(LoopDiagnosticsEndpoint, EmptyTableStillRendersParsableJson)
    {
        const ConfigValue parsed = parseBody(formatLoopDiagnosticsJson({}, 2, kRenderMoment));
        EXPECT_EQ(parsed.at("unregisteredLoopCount"), 2);
        EXPECT_TRUE(parsed.at("loops").empty());
    }

    /**
     * @brief 钉住：路径不以 / 开头属于用法错误，不能静默注册一条永不匹配的路由
     */
    TEST(LoopDiagnosticsEndpoint, RejectsPathWithoutLeadingSlash)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));

        EXPECT_THROW(server.enableLoopDiagnosticsEndpoint("debug/loops"), Base::InvalidArgumentException);
        EXPECT_THROW(server.enableLoopDiagnosticsEndpoint(""), Base::InvalidArgumentException);
    }

    /**
     * @brief 端到端：回环上真抓一次 /debug/loops，验状态码、内容类型与进程内的循环都进了表
     * @note 不钉 stalled：抓取那一条请求自身就在工作段里，机器慢的时候这一相超过阈值是合法结果，
     *       钉它等于把调度时序写进断言。判定逻辑由上面那条纯函数用例负责
     */
    TEST(LoopDiagnosticsEndpoint, ServesLoopDiagnosticsOverLoopback)
    {
        const ServerConfigurator configureServer = [](TestHttpServer &server)
        { server.enableLoopDiagnosticsEndpoint(); };
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{}, {}, HttpParserLimits{}, configureServer);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环";

        LoopbackClient client(fixture.listeningPort());
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        std::string receivedText;
        ASSERT_TRUE(client.sendText("GET /debug/loops HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", kEndpointTimeout));
        ASSERT_TRUE(client.waitForText(receivedText, "\"loops\":[", kEndpointTimeout)) << "观测端点没给出循环表：" << receivedText;

        const std::size_t bodyBegin = receivedText.find("\r\n\r\n");
        ASSERT_NE(bodyBegin, std::string::npos);
        const ConfigValue parsed = parseBody(receivedText.substr(bodyBegin + 4));
        EXPECT_NE(receivedText.find("HTTP/1.1 200"), std::string::npos) << receivedText;
        EXPECT_NE(receivedText.find("application/json"), std::string::npos) << receivedText;

        EXPECT_EQ(parsed.at("stallThresholdMicroseconds"), 100000);
        // 服务器在跑，表里就至少要有承载它的那条循环：这一条钉的是「循环确实登记了」而不是渲染格式
        EXPECT_GE(parsed.at("loops").size(), 1U) << "进程里有循环在跑，观测表却是空的";
        EXPECT_EQ(parsed.at("unregisteredLoopCount"), 0);
        bool hasRunningLoop = false;
        for (const ConfigValue &row: parsed.at("loops"))
        {
            hasRunningLoop = hasRunningLoop || row.at("running").get<bool>();
        }
        EXPECT_TRUE(hasRunningLoop) << "整表里没有一条在跑的循环：" << receivedText;
    }

} // namespace AsynGyanis::Net
