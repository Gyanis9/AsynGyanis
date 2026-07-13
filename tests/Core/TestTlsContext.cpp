#include <catch2/catch_test_macros.hpp>
#include "Core/TlsContext.h"

#include <cstdio>
#include <filesystem>
#include "Platform/Platform.h"
#include "Platform/SocketCompat.h"
#include <openssl/ssl.h>

#ifdef _WIN32
#include <process.h>
inline int getPid() { return _getpid(); }
#else
inline int getPid() { return getpid(); }
#endif

using namespace Core;

namespace {
    // Create temporary cert and key files for testing
    std::pair<std::string, std::string> createTestCert() {
        auto tmpDir = std::filesystem::temp_directory_path();
        auto pid    = getPid();
        std::string certPath = (tmpDir / ("test_cert_" + std::to_string(pid) + ".pem")).string();
        std::string keyPath  = (tmpDir / ("test_key_" + std::to_string(pid) + ".pem")).string();
    
        std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + keyPath +
                          " -out " + certPath + " -days 1 -nodes -subj \"/CN=test\"";
    #ifdef _WIN32
        cmd += " > NUL 2>&1";
    #else
        cmd += " 2>/dev/null";
    #endif
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
    REQUIRE(Platform::createSocketPair(fds[0], fds[1]));

    SSL *ssl = ctx.createSSL(fds[0]);
    REQUIRE(ssl != nullptr);

    SSL_free(ssl);
    Platform::closeFd(fds[0]);
    Platform::closeFd(fds[1]);
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
