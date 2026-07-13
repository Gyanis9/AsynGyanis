#include <catch2/catch_test_macros.hpp>
#include "Net/HttpResponse.h"

using namespace Net;

TEST_CASE("HttpResponse: default construction has 200 status", "[HttpResponse]") {
    HttpResponse res;
    REQUIRE(res.status() == 200);
    REQUIRE(res.body().empty());
    REQUIRE(res.headers().empty());
}

TEST_CASE("HttpResponse: setStatus and status", "[HttpResponse]") {
    HttpResponse res;
    res.setStatus(404);
    REQUIRE(res.status() == 404);
    res.setStatus(500);
    REQUIRE(res.status() == 500);
}

TEST_CASE("HttpResponse: setHeader and headers", "[HttpResponse]") {
    HttpResponse res;
    res.setHeader("Content-Type", "application/json");
    res.setHeader("X-Custom", "custom-value");

    REQUIRE(res.headers().size() == 2);
    REQUIRE(res.headers().at("Content-Type") == "application/json");
    REQUIRE(res.headers().at("X-Custom") == "custom-value");
}

TEST_CASE("HttpResponse: setHeader overwrites existing", "[HttpResponse]") {
    HttpResponse res;
    res.setHeader("Content-Type", "text/html");
    res.setHeader("Content-Type", "application/json");

    REQUIRE(res.headers().size() == 1);
    REQUIRE(res.headers().at("Content-Type") == "application/json");
}

TEST_CASE("HttpResponse: setBody and body", "[HttpResponse]") {
    HttpResponse res;
    res.setBody("hello world");
    REQUIRE(res.body() == "hello world");
}

TEST_CASE("HttpResponse: setBody with empty string", "[HttpResponse]") {
    HttpResponse res;
    res.setBody("");
    REQUIRE(res.body().empty());
}

TEST_CASE("HttpResponse: toString contains status line", "[HttpResponse]") {
    HttpResponse res;
    res.setStatus(200);
    res.setBody("OK");

    std::string str = res.toString();
    REQUIRE(str.find("HTTP/1.1 200") != std::string::npos);
    REQUIRE(str.find("OK") != std::string::npos);
}

TEST_CASE("HttpResponse: toString contains headers", "[HttpResponse]") {
    HttpResponse res;
    res.setStatus(200);
    res.setHeader("Content-Type", "text/plain");
    res.setBody("hello");

    std::string str = res.toString();
    REQUIRE(str.find("Content-Type: text/plain") != std::string::npos);
}

TEST_CASE("HttpResponse: toString contains body after blank line", "[HttpResponse]") {
    HttpResponse res;
    res.setStatus(200);
    res.setBody("response body");

    std::string str = res.toString();
    auto headerEnd = str.find("\r\n\r\n");
    REQUIRE(headerEnd != std::string::npos);
    REQUIRE(str.substr(headerEnd + 4) == "response body");
}

TEST_CASE("HttpResponse: ok factory creates 200 response", "[HttpResponse]") {
    auto res = HttpResponse::ok("success body");
    REQUIRE(res.status() == 200);
    REQUIRE(res.body() == "success body");
}

TEST_CASE("HttpResponse: notFound factory creates 404 response", "[HttpResponse]") {
    auto res = HttpResponse::notFound();
    REQUIRE(res.status() == 404);
}

TEST_CASE("HttpResponse: serverError factory creates 500 response", "[HttpResponse]") {
    auto res = HttpResponse::serverError("internal error");
    REQUIRE(res.status() == 500);
    REQUIRE(res.body() == "internal error");
}

TEST_CASE("HttpResponse: serverError with default message", "[HttpResponse]") {
    auto res = HttpResponse::serverError();
    REQUIRE(res.status() == 500);
}

TEST_CASE("HttpResponse: reset clears to initial state", "[HttpResponse]") {
    HttpResponse res;
    res.setStatus(404);
    res.setHeader("Content-Type", "text/html");
    res.setBody("not found");

    res.reset();

    REQUIRE(res.status() == 200);
    REQUIRE(res.body().empty());
    REQUIRE(res.headers().empty());
}

TEST_CASE("HttpResponse: setHttpVersion", "[HttpResponse]") {
    HttpResponse res;
    res.setHttpVersion("HTTP/2.0");
    res.setStatus(200);
    res.setBody("test");

    std::string str = res.toString();
    REQUIRE(str.find("HTTP/2.0 200") != std::string::npos);
}
