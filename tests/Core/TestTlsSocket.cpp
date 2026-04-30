#include <catch2/catch_test_macros.hpp>
#include "Core/TlsSocket.h"
#include "Core/TlsContext.h"
#include "Core/EventLoop.h"
#include "Core/AsyncSocket.h"
#include "Core/Scheduler.h"
#include "Core/Task.h"

#include <fstream>
#include <sys/socket.h>
#include <unistd.h>

using namespace Core;

namespace {
    std::pair<std::string, std::string> createTestCert() {
        std::string certPath = "/tmp/test_tls_cert_" + std::to_string(getpid()) + ".pem";
        std::string keyPath  = "/tmp/test_tls_key_" + std::to_string(getpid()) + ".pem";
        std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + keyPath +
                          " -out " + certPath + " -days 1 -nodes -subj \"/CN=test\" 2>/dev/null";
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
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

    SSL *ssl = ctx.createSSL(fds[0]);
    REQUIRE(ssl != nullptr);

    TlsSocket tls(ssl, loop, AsyncSocket(loop, fds[0]));
    REQUIRE(tls.fd() == fds[0]);

    tls.close();
    close(fds[1]);
    std::remove(cert.c_str());
    std::remove(key.c_str());
}

TEST_CASE("TlsSocket: move construction", "[TlsSocket]") {
    EventLoop loop;
    auto [cert, key] = createTestCert();
    TlsContext ctx;
    ctx.loadCertificate(cert, key);

    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

    SSL *ssl = ctx.createSSL(fds[0]);
    TlsSocket tls1(ssl, loop, AsyncSocket(loop, fds[0]));
    int fd = tls1.fd();

    TlsSocket tls2(std::move(tls1));
    REQUIRE(tls2.fd() == fd);

    tls2.close();
    close(fds[1]);
    std::remove(cert.c_str());
    std::remove(key.c_str());
}

TEST_CASE("TlsSocket: move assignment", "[TlsSocket]") {
    EventLoop loop;
    auto [cert, key] = createTestCert();
    TlsContext ctx;
    ctx.loadCertificate(cert, key);

    int fds1[2], fds2[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds1);
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds2);

    SSL *ssl1 = ctx.createSSL(fds1[0]);
    SSL *ssl2 = ctx.createSSL(fds2[0]);

    TlsSocket tls1(ssl1, loop, AsyncSocket(loop, fds1[0]));
    TlsSocket tls2(ssl2, loop, AsyncSocket(loop, fds2[0]));

    int fd1 = tls1.fd();
    tls2 = std::move(tls1);

    REQUIRE(tls2.fd() == fd1);

    tls2.close();
    close(fds1[1]);
    close(fds2[1]);
    std::remove(cert.c_str());
    std::remove(key.c_str());
}

TEST_CASE("TlsSocket: close safely", "[TlsSocket]") {
    EventLoop loop;
    auto [cert, key] = createTestCert();
    TlsContext ctx;
    ctx.loadCertificate(cert, key);

    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

    SSL *ssl = ctx.createSSL(fds[0]);
    TlsSocket tls(ssl, loop, AsyncSocket(loop, fds[0]));

    tls.close();
    // Double close should be safe
    REQUIRE_NOTHROW(tls.close());

    close(fds[1]);
    std::remove(cert.c_str());
    std::remove(key.c_str());
}

TEST_CASE("TlsSocket: SSL context creation and socket wrapping", "[TlsSocket]") {
    EventLoop loop;
    auto [cert, key] = createTestCert();
    TlsContext srvCtx;
    REQUIRE(srvCtx.loadCertificate(cert, key));

    int fds[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    SSL *ssl = srvCtx.createSSL(fds[0]);
    REQUIRE(ssl != nullptr);

    TlsSocket tls(ssl, loop, AsyncSocket(loop, fds[0]));
    REQUIRE(tls.fd() == fds[0]);

    tls.close();
    close(fds[1]);
    std::remove(cert.c_str());
    std::remove(key.c_str());
}
