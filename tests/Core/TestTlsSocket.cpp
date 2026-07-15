#include <catch2/catch_test_macros.hpp>
#include "Core/TlsSocket.h"
#include "Core/TlsContext.h"
#include "Core/EventLoop.h"
#include "Core/AsyncSocket.h"
#include "Core/Scheduler.h"
#include "Core/Task.h"

#include <fstream>
#include <filesystem>
#include "Platform/Platform.h"
#include "Platform/SocketCompat.h"

#ifdef _WIN32
#include <process.h>
inline int getPid() { return _getpid(); }
#else
inline int getPid() { return getpid(); }
#endif

using namespace Core;

namespace {
    std::pair<std::string, std::string> createTestCert() {
        auto tmpDir = std::filesystem::temp_directory_path();
        auto pid    = getPid();
        std::string certPath = (tmpDir / ("test_tls_cert_" + std::to_string(pid) + ".pem")).string();
        std::string keyPath  = (tmpDir / ("test_tls_key_" + std::to_string(pid) + ".pem")).string();
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

TEST_CASE("TlsSocket: construction", "[TlsSocket]") {
    EventLoop loop;
    auto [cert, key] = createTestCert();
    TlsContext ctx;
    ctx.loadCertificate(cert, key);

    int fds[2];
    Platform::createSocketPair(fds[0], fds[1]);

    SSL *ssl = ctx.createSSL(fds[0]);
    REQUIRE(ssl != nullptr);

    TlsSocket tls(ssl, loop, AsyncSocket(loop, fds[0]));
    REQUIRE(tls.fileDescriptor() == fds[0]);

    tls.close();
    Platform::closeFileDescriptor(fds[1]);
    std::remove(cert.c_str());
    std::remove(key.c_str());
}

TEST_CASE("TlsSocket: move construction", "[TlsSocket]") {
    EventLoop loop;
    auto [cert, key] = createTestCert();
    TlsContext ctx;
    ctx.loadCertificate(cert, key);

    int fds[2];
    Platform::createSocketPair(fds[0], fds[1]);

    SSL *ssl = ctx.createSSL(fds[0]);
    TlsSocket tls1(ssl, loop, AsyncSocket(loop, fds[0]));
    int fd = tls1.fileDescriptor();

    TlsSocket tls2(std::move(tls1));
    REQUIRE(tls2.fileDescriptor() == fd);

    tls2.close();
    Platform::closeFileDescriptor(fds[1]);
    std::remove(cert.c_str());
    std::remove(key.c_str());
}

TEST_CASE("TlsSocket: move assignment", "[TlsSocket]") {
    EventLoop loop;
    auto [cert, key] = createTestCert();
    TlsContext ctx;
    ctx.loadCertificate(cert, key);

    int fds1[2], fds2[2];
    Platform::createSocketPair(fds1[0], fds1[1]);
    Platform::createSocketPair(fds2[0], fds2[1]);

    SSL *ssl1 = ctx.createSSL(fds1[0]);
    SSL *ssl2 = ctx.createSSL(fds2[0]);

    TlsSocket tls1(ssl1, loop, AsyncSocket(loop, fds1[0]));
    TlsSocket tls2(ssl2, loop, AsyncSocket(loop, fds2[0]));

    int fd1 = tls1.fileDescriptor();
    tls2 = std::move(tls1);

    REQUIRE(tls2.fileDescriptor() == fd1);

    tls2.close();
    Platform::closeFileDescriptor(fds1[1]);
    Platform::closeFileDescriptor(fds2[1]);
    std::remove(cert.c_str());
    std::remove(key.c_str());
}

TEST_CASE("TlsSocket: close safely", "[TlsSocket]") {
    EventLoop loop;
    auto [cert, key] = createTestCert();
    TlsContext ctx;
    ctx.loadCertificate(cert, key);

    int fds[2];
    Platform::createSocketPair(fds[0], fds[1]);

    SSL *ssl = ctx.createSSL(fds[0]);
    TlsSocket tls(ssl, loop, AsyncSocket(loop, fds[0]));

    tls.close();
    // 双重关闭应该是安全的
    REQUIRE_NOTHROW(tls.close());

    Platform::closeFileDescriptor(fds[1]);
    std::remove(cert.c_str());
    std::remove(key.c_str());
}

TEST_CASE("TlsSocket: SSL context creation and socket wrapping", "[TlsSocket]") {
    EventLoop loop;
    auto [cert, key] = createTestCert();
    TlsContext srvCtx;
    REQUIRE(srvCtx.loadCertificate(cert, key));

    int fds[2];
    REQUIRE(Platform::createSocketPair(fds[0], fds[1]));

    SSL *ssl = srvCtx.createSSL(fds[0]);
    REQUIRE(ssl != nullptr);

    TlsSocket tls(ssl, loop, AsyncSocket(loop, fds[0]));
    REQUIRE(tls.fileDescriptor() == fds[0]);

    tls.close();
    Platform::closeFileDescriptor(fds[1]);
    std::remove(cert.c_str());
    std::remove(key.c_str());
}
