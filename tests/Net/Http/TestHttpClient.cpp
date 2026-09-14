/**
 * @file TestHttpClient.cpp
 * @brief HTTP 出站客户端端到端用例
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#include "HttpTestSupport.h"
#include "Net/Http/Client/HttpClient.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;
        constexpr auto kTimeout = std::chrono::seconds{10};

        Core::Task<void> doGetTask(Core::EventLoop &loop,
                                   std::unique_ptr<HttpClientResponse> &result,
                                   std::string url)
        {
            result = co_await HttpClient::get(loop, url);
            loop.stop();
        }

        static std::unique_ptr<HttpClientResponse> doGet(std::string_view url)
        {
            Core::EventLoop loop;
            std::unique_ptr<HttpClientResponse> result;
            auto work = doGetTask(loop, result, std::string(url));
            if (!work.isReady()) loop.scheduler().schedule(work.handle());
            loop.run();
            return result;
        }
    }

    TEST(HttpClientUrl, ParsesSimpleUrl)
    {
        auto u = parseUrl("http://example.com/path");
        EXPECT_EQ(u.scheme, "http");  EXPECT_EQ(u.host, "example.com");
        EXPECT_EQ(u.port, 80);        EXPECT_EQ(u.path, "/path");
    }

    TEST(HttpClientUrl, ParsesUrlWithPort)
    {
        auto u = parseUrl("http://localhost:8080/test");
        EXPECT_EQ(u.host, "localhost"); EXPECT_EQ(u.port, 8080); EXPECT_EQ(u.path, "/test");
    }

    TEST(HttpClientUrl, ParsesHttpsScheme)
    {
        auto u = parseUrl("https://api.example.com/v1/data");
        EXPECT_EQ(u.scheme, "https"); EXPECT_EQ(u.port, 443);
    }

    TEST(HttpClientUrl, DefaultsPathToSlash)
    {
        auto u = parseUrl("http://example.com");
        EXPECT_EQ(u.path, "/");
    }

    TEST(HttpClient, GetsLocalhostAndReceives200)
    {
        auto fixture = std::make_unique<RunningHttpServerFixture>(
                HttpServerLimits{}, std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture->awaitRunning(kTimeout));
        auto port = fixture->listeningPort();  ASSERT_NE(port, 0U);
        auto url = "http://127.0.0.1:" + std::to_string(port) + "/hello";
        auto r = doGet(url);
        ASSERT_NE(r, nullptr) << "请求失败";
        EXPECT_EQ(r->statusCode, 200);
        EXPECT_NE(r->body.find("served-hello"), std::string::npos) << "正文：" << r->body;
    }
} // namespace AsynGyanis::Net