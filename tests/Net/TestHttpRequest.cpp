#include <catch2/catch_test_macros.hpp>
#include "Net/HttpRequest.h"

using namespace Net;

TEST_CASE("HttpRequest: default construction", "[HttpRequest]") {
    HttpRequest request;
    REQUIRE(request.method() == HttpMethod::UNKNOWN);
    REQUIRE(request.uri().empty());
    REQUIRE(request.httpVersion().empty());
    REQUIRE(request.body().empty());
    REQUIRE(request.headers().empty());
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
    HttpRequest request;
    request.setMethod(HttpMethod::GET);
    REQUIRE(request.method() == HttpMethod::GET);
    request.setMethod(HttpMethod::POST);
    REQUIRE(request.method() == HttpMethod::POST);
}

TEST_CASE("HttpRequest: setUri and uri", "[HttpRequest]") {
    HttpRequest request;
    request.setUri("/api/users/123");
    REQUIRE(request.uri() == "/api/users/123");
}

TEST_CASE("HttpRequest: path extracts path without query", "[HttpRequest]") {
    HttpRequest request;
    request.setUri("/api/users?id=42&name=hello");
    REQUIRE(request.path() == "/api/users");
}

TEST_CASE("HttpRequest: path returns full uri when no query", "[HttpRequest]") {
    HttpRequest request;
    request.setUri("/api/users/123");
    REQUIRE(request.path() == "/api/users/123");
}

TEST_CASE("HttpRequest: addHeader and getHeader", "[HttpRequest]") {
    HttpRequest request;
    request.addHeader("Content-Type", "application/json");
    request.addHeader("Authorization", "Bearer token123");

    auto ct = request.getHeader("Content-Type");
    REQUIRE(ct.has_value());
    REQUIRE(*ct == "application/json");

    auto auth = request.getHeader("authorization");
    REQUIRE(auth.has_value());
    REQUIRE(*auth == "Bearer token123");

    REQUIRE_FALSE(request.getHeader("Nonexistent").has_value());
}

TEST_CASE("HttpRequest: addHeader is case-insensitive", "[HttpRequest]") {
    HttpRequest request;
    request.addHeader("Content-Type", "text/html");

    REQUIRE(request.getHeader("content-type").has_value());
    REQUIRE(request.getHeader("CONTENT-TYPE").has_value());
    REQUIRE(request.getHeader("Content-Type").has_value());
}

TEST_CASE("HttpRequest: duplicate headers are comma-joined", "[HttpRequest]") {
    HttpRequest request;
    request.addHeader("X-Custom", "value1");
    request.addHeader("X-Custom", "value2");

    auto h = request.getHeader("X-Custom");
    REQUIRE(h.has_value());
    REQUIRE(*h == "value1, value2");
}

TEST_CASE("HttpRequest: setBody and body", "[HttpRequest]") {
    HttpRequest request;
    request.setBody("hello world");
    REQUIRE(request.body() == "hello world");
}

TEST_CASE("HttpRequest: appendBody", "[HttpRequest]") {
    HttpRequest request;
    request.setBody("hello");
    request.appendBody(" world", 6);
    REQUIRE(request.body() == "hello world");
}

TEST_CASE("HttpRequest: queryParams parses simple params", "[HttpRequest]") {
    HttpRequest request;
    request.setUri("/search?q=hello&page=1");

    auto params = request.queryParams();
    REQUIRE(params.size() == 2);
    REQUIRE(params["q"] == "hello");
    REQUIRE(params["page"] == "1");
}

TEST_CASE("HttpRequest: queryParams handles URL encoding", "[HttpRequest]") {
    HttpRequest request;
    request.setUri("/search?q=hello%20world&name=John+Doe");

    auto params = request.queryParams();
    REQUIRE(params["q"] == "hello world");
    REQUIRE(params["name"] == "John Doe");
}

TEST_CASE("HttpRequest: queryParams empty when no query string", "[HttpRequest]") {
    HttpRequest request;
    request.setUri("/api/users");

    auto params = request.queryParams();
    REQUIRE(params.empty());
}

TEST_CASE("HttpRequest: setParam and param", "[HttpRequest]") {
    HttpRequest request;
    request.setParam("id", "42");
    request.setParam("name", "alice");

    REQUIRE(request.param("id").has_value());
    REQUIRE(*request.param("id") == "42");
    REQUIRE(request.param("name").has_value());
    REQUIRE(*request.param("name") == "alice");
    REQUIRE_FALSE(request.param("nonexistent").has_value());
}

TEST_CASE("HttpRequest: reset clears all fields", "[HttpRequest]") {
    HttpRequest request;
    request.setMethod(HttpMethod::GET);
    request.setUri("/api/test");
    request.setHttpVersion("HTTP/1.1");
    request.addHeader("Content-Type", "application/json");
    request.setBody("body content");
    request.setParam("key", "value");

    request.reset();

    REQUIRE(request.method() == HttpMethod::UNKNOWN);
    REQUIRE(request.uri().empty());
    REQUIRE(request.httpVersion().empty());
    REQUIRE(request.headers().empty());
    REQUIRE(request.body().empty());
}

TEST_CASE("HttpRequest: setHttpVersion and httpVersion", "[HttpRequest]") {
    HttpRequest request;
    request.setHttpVersion("HTTP/1.1");
    REQUIRE(request.httpVersion() == "HTTP/1.1");
    request.setHttpVersion("HTTP/2.0");
    REQUIRE(request.httpVersion() == "HTTP/2.0");
}

TEST_CASE("HttpRequest: cancelToken initially not stop_requested", "[HttpRequest]") {
    HttpRequest request;
    auto token = request.cancelToken();
    REQUIRE_FALSE(token.stop_requested());
}

TEST_CASE("HttpRequest: requestCancel sets stop_requested", "[HttpRequest]") {
    HttpRequest request;
    REQUIRE(request.requestCancel());
    auto token = request.cancelToken();
    REQUIRE(token.stop_requested());
}
