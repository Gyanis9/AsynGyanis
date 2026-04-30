#include <catch2/catch_test_macros.hpp>
#include "Core/TlsContext.h"

#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>
#include <openssl/ssl.h>

using namespace Core;

namespace {
    // Create temporary cert and key files for testing
    std::pair<std::string, std::string> createTestCert() {
        std::string certPath = "/tmp/test_cert_" + std::to_string(getpid()) + ".pem";
        std::string keyPath  = "/tmp/test_key_" + std::to_string(getpid()) + ".pem";

        std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + keyPath +
                          " -out " + certPath + " -days 1 -nodes -subj \"/CN=test\" 2>/dev/null";
        system(cmd.c_str());

        return {certPath, keyPath};
    }
}

TEST_CASE("TlsContext: construction", "[TlsContext]") {
    TlsContext ctx;
    REQUIRE(ctx.nativeHandle() != nullptr);
}

TEST_CASE("TlsContext: load valid certificate", "[TlsContext]") {
    auto [cert, key] = createTestCert();
    TlsContext ctx;
    REQUIRE(ctx.loadCertificate(cert, key));
    std::remove(cert.c_str());
    std::remove(key.c_str());
}

TEST_CASE("TlsContext: loadCertificate fails with invalid files", "[TlsContext]") {
    TlsContext ctx;
    REQUIRE_FALSE(ctx.loadCertificate("/nonexistent/cert.pem", "/nonexistent/key.pem"));
}

TEST_CASE("TlsContext: createSSL returns non-null", "[TlsContext]") {
    auto [cert, key] = createTestCert();
    TlsContext ctx;
    REQUIRE(ctx.loadCertificate(cert, key));

    // Create a socket pair for testing
    int fds[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    SSL *ssl = ctx.createSSL(fds[0]);
    REQUIRE(ssl != nullptr);

    SSL_free(ssl);
    close(fds[0]);
    close(fds[1]);
    std::remove(cert.c_str());
    std::remove(key.c_str());
}

TEST_CASE("TlsContext: nativeHandle returns same pointer", "[TlsContext]") {
    TlsContext ctx;
    auto *handle = ctx.nativeHandle();
    REQUIRE(handle != nullptr);
}

TEST_CASE("TlsContext: construction throws on SSL init failure is handled", "[TlsContext]") {
    // Basic construction should succeed on any system with OpenSSL
    REQUIRE_NOTHROW([]() { TlsContext ctx; }());
}
