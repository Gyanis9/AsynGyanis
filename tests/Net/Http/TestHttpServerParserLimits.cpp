// TestHttpServerParserLimits.cpp —— HttpServer::setParserLimits 的系统级覆盖：
//   一. 生效：限额调小后，越界请求按类别回 431/413（而不是笼统的 400），
//      且同一台服务器上未越界的请求仍被正常服务（限额只卡越界的那一条报文）；
//   二. 访问器：parserLimits() 返回最后一次落定的配置，含 0 这类边界取值；
//   三. 拒绝面：某项设为 0 表示关闭该项保护，超出出厂默认档口的请求照样被服务。
// 用例走真实回环套接字，端口由内核分配，用例之间不共用任何固定资源；
// 回环夹具（RunningHttpServerFixture / LoopbackClient）在 HttpTestSupport.h 中。

#include "Net/Http/HttpServer.h"

#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "HttpTestSupport.h"
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/Router.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace AsynGyanis::Net
{
    // 回环夹具与客户端集中在本头文件里，与服务端限额、观测性用例共用一份实现
    using namespace HttpTestSupport;

    /**
     * @brief 头部值上限生效：越界的头部回 431（不是 400），同机未越界的请求照常拿到 200
     */
    TEST(HttpServerParserLimits, SmallHeaderValueLimitAnswers431AndKeepsServingNormalRequests)
    {
        HttpParserLimits parserLimits;
        parserLimits.maximumHeaderFieldValueLength = 32;

        // 连接级限额保持缺省（三项超时都很长）：本用例只观察解析上限，不该被超时收口干扰
        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{50}, {}, {}, parserLimits);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";
        EXPECT_FALSE(fixture.startThrew());
        EXPECT_EQ(fixture.server().parserLimits().maximumHeaderFieldValueLength, 32u) << "落定的解析上限与传入值不符";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        // 越界的那一条：单个头部值 64 字节 > 32，属「形态合法、体量越界」，必须回 431
        {
            LoopbackClient client(listeningPort);
            ASSERT_TRUE(client.isValid()) << "回环连接失败";
            const std::string request = makeRequestText("GET /hello HTTP/1.1", {"x-blob: " + std::string(64, 'a')});
            ASSERT_TRUE(client.sendText(request, kWaitTimeout)) << "越界请求未能写入";

            std::string responseText;
            ASSERT_TRUE(client.waitForText(responseText, "Request Header Fields Too Large", kWaitTimeout))
                    << "越界头部未在时限内被判 431：上界 kWaitTimeout";
            EXPECT_NE(responseText.find("HTTP/1.1 431"), std::string::npos) << responseText;
            EXPECT_EQ(responseText.find("HTTP/1.1 400"), std::string::npos) << "越界被当成了协议级非法：" << responseText;
            EXPECT_NE(responseText.find("connection: close"), std::string::npos) << responseText;
            EXPECT_TRUE(client.waitForClosure(responseText, kWaitTimeout)) << "回完 431 没有收口";
        }

        // 同一台服务器上的下一条连接：一切正常的请求必须照常被服务
        {
            LoopbackClient normalClient(listeningPort);
            ASSERT_TRUE(normalClient.isValid()) << "回环连接失败";
            ASSERT_TRUE(normalClient.sendText(helloRequestText(), kWaitTimeout)) << "正常请求未能写入";

            std::string responseText;
            EXPECT_TRUE(normalClient.waitForText(responseText, "served-hello", kWaitTimeout))
                    << "同一台服务器上未超限的请求未被正常服务：" << responseText;
            EXPECT_NE(responseText.find("HTTP/1.1 200"), std::string::npos) << responseText;
        }
    }

    /**
     * @brief 正文上限生效：声明长度越界回 413（不是 400），且同机正常请求仍被服务
     */
    TEST(HttpServerParserLimits, SmallBodyLimitAnswers413AndKeepsServingNormalRequests)
    {
        HttpParserLimits parserLimits;
        parserLimits.maximumBodySize = 16;

        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{50}, {}, {}, parserLimits);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";
        EXPECT_EQ(fixture.server().parserLimits().maximumBodySize, 16u) << "落定的解析上限与传入值不符";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        // 声明的正文长度 64 字节 > 16：定界器在解析头部那一刻就拦，正文一个字节都不必发
        {
            LoopbackClient client(listeningPort);
            ASSERT_TRUE(client.isValid()) << "回环连接失败";
            const std::string request =
                    makeRequestText("POST /hello HTTP/1.1", {"content-length: 64", "content-type: text/plain"});
            ASSERT_TRUE(client.sendText(request, kWaitTimeout)) << "越界请求未能写入";

            std::string responseText;
            ASSERT_TRUE(client.waitForText(responseText, "Payload Too Large", kWaitTimeout))
                    << "超限声明未在时限内被判 413：上界 kWaitTimeout";
            EXPECT_NE(responseText.find("HTTP/1.1 413"), std::string::npos) << responseText;
            EXPECT_EQ(responseText.find("HTTP/1.1 400"), std::string::npos) << "越界被当成了协议级非法：" << responseText;
            EXPECT_TRUE(client.waitForClosure(responseText, kWaitTimeout)) << "回完 413 没有收口";
        }

        // 同一台服务器上的下一条连接：GET 无正文，不受正文上限影响
        {
            LoopbackClient normalClient(listeningPort);
            ASSERT_TRUE(normalClient.isValid()) << "回环连接失败";
            ASSERT_TRUE(normalClient.sendText(helloRequestText(), kWaitTimeout)) << "正常请求未能写入";

            std::string responseText;
            EXPECT_TRUE(normalClient.waitForText(responseText, "served-hello", kWaitTimeout))
                    << "同一台服务器上未超限的请求未被正常服务：" << responseText;
        }
    }

    /**
     * @brief 头部条数上限设为 0（关闭该项保护）时，超过出厂档口的头部条数照样被服务
     *
     * @details 反向对照：出厂值 100 条。这里发 121 条，若 0 被理解成「一条都不许」或「仍按 100 条卡」，
     *          请求都会拿不到 200——因此本用例同时排除了这两种误读。
     */
    TEST(HttpServerParserLimits, ZeroHeaderCountLimitServesRequestsBeyondTheShippedDefault)
    {
        HttpParserLimits parserLimits;
        parserLimits.maximumHeaderCount = 0;

        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{50}, {}, {}, parserLimits);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";
        EXPECT_EQ(fixture.server().parserLimits().maximumHeaderCount, 0u) << "落定的解析上限与传入值不符";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        std::vector<std::string> headerLines;
        headerLines.reserve(120);
        for (std::size_t index = 0; index < 120; ++index)
        {
            headerLines.push_back("x-count-" + std::to_string(index) + ": v");
        }

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        const std::string request = makeRequestText("GET /hello HTTP/1.1", headerLines);
        ASSERT_TRUE(client.sendText(request, kWaitTimeout)) << "头部较多的请求未能写入";

        std::string responseText;
        EXPECT_TRUE(client.waitForText(responseText, "served-hello", kWaitTimeout))
                << "条数上限为 0 时 121 条头部仍被拒：" << responseText;
        EXPECT_NE(responseText.find("HTTP/1.1 200"), std::string::npos) << responseText;
    }
} // namespace AsynGyanis::Net
