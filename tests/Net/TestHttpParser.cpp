#include <catch2/catch_test_macros.hpp>
#include "Net/HttpParser.h"

#include <string>

using namespace Net;

TEST_CASE("HttpParser: parses a simple GET request", "[HttpParser]") {
    HttpParser        parser;
    const std::string raw = "GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n";

    const auto status = parser.parse(raw.data(), raw.size());

    REQUIRE(status == ParseStatus::Done);
    REQUIRE_FALSE(parser.hasError());
    REQUIRE(parser.request().method() == HttpMethod::GET);
    REQUIRE(parser.request().path() == "/index.html");
}

TEST_CASE("HttpParser: accepts a body within the size limit", "[HttpParser]") {
    HttpParser        parser;
    const std::string body = "hello world";
    const std::string raw  = "POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                          + std::to_string(body.size()) + "\r\n\r\n" + body;

    const auto status = parser.parse(raw.data(), raw.size());

    REQUIRE(status == ParseStatus::Done);
    REQUIRE_FALSE(parser.hasError());
    REQUIRE(parser.request().body() == "hello world");
}

TEST_CASE("HttpParser: rejects an overly long request URI (DoS guard)", "[HttpParser][security]") {
    HttpParser parser;
    // URI 长度上限为 8 KB，此处构造 9 KB 触发保护
    const std::string longPath(9 * 1024, 'a');
    const std::string raw = "GET /" + longPath + " HTTP/1.1\r\nHost: localhost\r\n\r\n";

    const auto status = parser.parse(raw.data(), raw.size());

    REQUIRE(status == ParseStatus::Error);
    REQUIRE(parser.hasError());
}

TEST_CASE("HttpParser: rejects a request body exceeding the size limit (DoS guard)", "[HttpParser][security]") {
    HttpParser parser;
    // 请求体上限为 8 MB，此处构造 9 MB 触发保护
    const size_t      bodySize = 9ull * 1024 * 1024;
    const std::string body(bodySize, 'x');
    const std::string raw = "POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                          + std::to_string(bodySize) + "\r\n\r\n" + body;

    const auto status = parser.parse(raw.data(), raw.size());

    REQUIRE(status == ParseStatus::Error);
    REQUIRE(parser.hasError());
}
