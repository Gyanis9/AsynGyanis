#include <catch2/catch_test_macros.hpp>
#include "Net/HttpResponse.h"

using namespace Net;

TEST_CASE("HttpResponse: default construction has 200 status", "[HttpResponse]") {
    HttpResponse response;
    REQUIRE(response.status() == 200);
    REQUIRE(response.body().empty());
    REQUIRE(response.headers().empty());
}

TEST_CASE("HttpResponse: setStatus and status", "[HttpResponse]") {
    HttpResponse response;
    response.setStatus(404);
    REQUIRE(response.status() == 404);
    response.setStatus(500);
    REQUIRE(response.status() == 500);
}

TEST_CASE("HttpResponse: setHeader and headers", "[HttpResponse]") {
    HttpResponse response;
    response.setHeader("Content-Type", "application/json");
    response.setHeader("X-Custom", "custom-value");

    REQUIRE(response.headers().size() == 2);
    REQUIRE(response.headers().at("content-type") == "application/json");
    REQUIRE(response.headers().at("x-custom") == "custom-value");
}

TEST_CASE("HttpResponse: setHeader overwrites existing", "[HttpResponse]") {
    HttpResponse response;
    response.setHeader("Content-Type", "text/html");
    response.setHeader("Content-Type", "application/json");

    REQUIRE(response.headers().size() == 1);
    REQUIRE(response.headers().at("content-type") == "application/json");
}

TEST_CASE("HttpResponse: setBody and body", "[HttpResponse]") {
    HttpResponse response;
    response.setBody("hello world");
    REQUIRE(response.body() == "hello world");
}

TEST_CASE("HttpResponse: setBody with empty string", "[HttpResponse]") {
    HttpResponse response;
    response.setBody("");
    REQUIRE(response.body().empty());
}

TEST_CASE("HttpResponse: toString contains status line", "[HttpResponse]") {
    HttpResponse response;
    response.setStatus(200);
    response.setBody("OK");

    std::string str = response.toString();
    REQUIRE(str.find("HTTP/1.1 200") != std::string::npos);
    REQUIRE(str.find("OK") != std::string::npos);
}

TEST_CASE("HttpResponse: toString contains headers", "[HttpResponse]") {
    HttpResponse response;
    response.setStatus(200);
    response.setHeader("Content-Type", "text/plain");
    response.setBody("hello");

    std::string str = response.toString();
    REQUIRE(str.find("content-type: text/plain") != std::string::npos);
}

TEST_CASE("HttpResponse: toString contains body after blank line", "[HttpResponse]") {
    HttpResponse response;
    response.setStatus(200);
    response.setBody("response body");

    std::string str = response.toString();
    auto headerEnd = str.find("\r\n\r\n");
    REQUIRE(headerEnd != std::string::npos);
    REQUIRE(str.substr(headerEnd + 4) == "response body");
}

TEST_CASE("HttpResponse: ok factory creates 200 response", "[HttpResponse]") {
    auto response = HttpResponse::ok("success body");
    REQUIRE(response.status() == 200);
    REQUIRE(response.body() == "success body");
}

TEST_CASE("HttpResponse: notFound factory creates 404 response", "[HttpResponse]") {
    auto response = HttpResponse::notFound();
    REQUIRE(response.status() == 404);
}

TEST_CASE("HttpResponse: serverError factory creates 500 response", "[HttpResponse]") {
    auto response = HttpResponse::serverError("internal error");
    REQUIRE(response.status() == 500);
    REQUIRE(response.body() == "internal error");
}

TEST_CASE("HttpResponse: serverError with default message", "[HttpResponse]") {
    auto response = HttpResponse::serverError();
    REQUIRE(response.status() == 500);
}

TEST_CASE("HttpResponse: reset clears to initial state", "[HttpResponse]") {
    HttpResponse response;
    response.setStatus(404);
    response.setHeader("Content-Type", "text/html");
    response.setBody("not found");

    response.reset();

    REQUIRE(response.status() == 200);
    REQUIRE(response.body().empty());
    REQUIRE(response.headers().empty());
}

TEST_CASE("HttpResponse: setHttpVersion", "[HttpResponse]") {
    HttpResponse response;
    response.setHttpVersion("HTTP/2.0");
    response.setStatus(200);
    response.setBody("test");

    std::string str = response.toString();
    REQUIRE(str.find("HTTP/2.0 200") != std::string::npos);
}
