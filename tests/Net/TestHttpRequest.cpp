#include <catch2/catch_test_macros.hpp>
#include "Net/HttpRequest.h"

using namespace Net;

TEST_CASE("HttpRequest: default construction", "[HttpRequest]") {
    HttpRequest req;
    REQUIRE(req.method() == HttpMethod::UNKNOWN);
    REQUIRE(req.uri().empty());
    REQUIRE(req.httpVersion().empty());
    REQUIRE(req.body().empty());
    REQUIRE(req.headers().empty());
}

TEST_CASE("HttpRequest: methodFromString parses all methods", "[HttpRequest]") {
    REQUIRE(HttpRequest::methodFromString("GET") == HttpMethod::GET);
    REQUIRE(HttpRequest::methodFromString("POST") == HttpMethod::POST);
    REQUIRE(HttpRequest::methodFromString("PUT") == HttpMethod::PUT);
    REQUIRE(HttpRequest::methodFromString("DELETE") == HttpMethod::DELETE);
    REQUIRE(HttpRequest::methodFromString("PATCH") == HttpMethod::PATCH);
    REQUIRE(HttpRequest::methodFromString("HEAD") == HttpMethod::HEAD);
    REQUIRE(HttpRequest::methodFromString("OPTIONS") == HttpMethod::OPTIONS);
}

TEST_CASE("HttpRequest: methodFromString returns UNKNOWN for invalid", "[HttpRequest]") {
    REQUIRE(HttpRequest::methodFromString("INVALID") == HttpMethod::UNKNOWN);
    REQUIRE(HttpRequest::methodFromString("") == HttpMethod::UNKNOWN);
    REQUIRE(HttpRequest::methodFromString("get") == HttpMethod::UNKNOWN);
}

TEST_CASE("HttpRequest: setMethod and method", "[HttpRequest]") {
    HttpRequest req;
    req.setMethod(HttpMethod::GET);
    REQUIRE(req.method() == HttpMethod::GET);
    req.setMethod(HttpMethod::POST);
    REQUIRE(req.method() == HttpMethod::POST);
}

TEST_CASE("HttpRequest: setUri and uri", "[HttpRequest]") {
    HttpRequest req;
    req.setUri("/api/users/123");
    REQUIRE(req.uri() == "/api/users/123");
}

TEST_CASE("HttpRequest: path extracts path without query", "[HttpRequest]") {
    HttpRequest req;
    req.setUri("/api/users?id=42&name=hello");
    REQUIRE(req.path() == "/api/users");
}

TEST_CASE("HttpRequest: path returns full uri when no query", "[HttpRequest]") {
    HttpRequest req;
    req.setUri("/api/users/123");
    REQUIRE(req.path() == "/api/users/123");
}

TEST_CASE("HttpRequest: addHeader and getHeader", "[HttpRequest]") {
    HttpRequest req;
    req.addHeader("Content-Type", "application/json");
    req.addHeader("Authorization", "Bearer token123");

    auto ct = req.getHeader("Content-Type");
    REQUIRE(ct.has_value());
    REQUIRE(*ct == "application/json");

    auto auth = req.getHeader("authorization");
    REQUIRE(auth.has_value());
    REQUIRE(*auth == "Bearer token123");

    REQUIRE_FALSE(req.getHeader("Nonexistent").has_value());
}

TEST_CASE("HttpRequest: addHeader is case-insensitive", "[HttpRequest]") {
    HttpRequest req;
    req.addHeader("Content-Type", "text/html");

    REQUIRE(req.getHeader("content-type").has_value());
    REQUIRE(req.getHeader("CONTENT-TYPE").has_value());
    REQUIRE(req.getHeader("Content-Type").has_value());
}

TEST_CASE("HttpRequest: duplicate headers are comma-joined", "[HttpRequest]") {
    HttpRequest req;
    req.addHeader("X-Custom", "value1");
    req.addHeader("X-Custom", "value2");

    auto h = req.getHeader("X-Custom");
    REQUIRE(h.has_value());
    REQUIRE(*h == "value1, value2");
}

TEST_CASE("HttpRequest: setBody and body", "[HttpRequest]") {
    HttpRequest req;
    req.setBody("hello world");
    REQUIRE(req.body() == "hello world");
}

TEST_CASE("HttpRequest: appendBody", "[HttpRequest]") {
    HttpRequest req;
    req.setBody("hello");
    req.appendBody(" world", 6);
    REQUIRE(req.body() == "hello world");
}

TEST_CASE("HttpRequest: queryParams parses simple params", "[HttpRequest]") {
    HttpRequest req;
    req.setUri("/search?q=hello&page=1");

    auto params = req.queryParams();
    REQUIRE(params.size() == 2);
    REQUIRE(params["q"] == "hello");
    REQUIRE(params["page"] == "1");
}

TEST_CASE("HttpRequest: queryParams handles URL encoding", "[HttpRequest]") {
    HttpRequest req;
    req.setUri("/search?q=hello%20world&name=John+Doe");

    auto params = req.queryParams();
    REQUIRE(params["q"] == "hello world");
    REQUIRE(params["name"] == "John Doe");
}

TEST_CASE("HttpRequest: queryParams empty when no query string", "[HttpRequest]") {
    HttpRequest req;
    req.setUri("/api/users");

    auto params = req.queryParams();
    REQUIRE(params.empty());
}

TEST_CASE("HttpRequest: setParam and param", "[HttpRequest]") {
    HttpRequest req;
    req.setParam("id", "42");
    req.setParam("name", "alice");

    REQUIRE(req.param("id").has_value());
    REQUIRE(*req.param("id") == "42");
    REQUIRE(req.param("name").has_value());
    REQUIRE(*req.param("name") == "alice");
    REQUIRE_FALSE(req.param("nonexistent").has_value());
}

TEST_CASE("HttpRequest: reset clears all fields", "[HttpRequest]") {
    HttpRequest req;
    req.setMethod(HttpMethod::GET);
    req.setUri("/api/test");
    req.setHttpVersion("HTTP/1.1");
    req.addHeader("Content-Type", "application/json");
    req.setBody("body content");
    req.setParam("key", "value");

    req.reset();

    REQUIRE(req.method() == HttpMethod::UNKNOWN);
    REQUIRE(req.uri().empty());
    REQUIRE(req.httpVersion().empty());
    REQUIRE(req.headers().empty());
    REQUIRE(req.body().empty());
}

TEST_CASE("HttpRequest: setHttpVersion and httpVersion", "[HttpRequest]") {
    HttpRequest req;
    req.setHttpVersion("HTTP/1.1");
    REQUIRE(req.httpVersion() == "HTTP/1.1");
    req.setHttpVersion("HTTP/2.0");
    REQUIRE(req.httpVersion() == "HTTP/2.0");
}

TEST_CASE("HttpRequest: cancelToken initially not stop_requested", "[HttpRequest]") {
    HttpRequest req;
    auto token = req.cancelToken();
    REQUIRE_FALSE(token.stop_requested());
}

TEST_CASE("HttpRequest: requestCancel sets stop_requested", "[HttpRequest]") {
    HttpRequest req;
    REQUIRE(req.requestCancel());
    auto token = req.cancelToken();
    REQUIRE(token.stop_requested());
}
