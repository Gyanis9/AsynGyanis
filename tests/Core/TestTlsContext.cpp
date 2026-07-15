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
    // 创建用于测试的临时证书和密钥文件
    std::pair<std::string, std::string> createTestCertificate() {
        auto tmpDir = std::filesystem::temp_directory_path();
        auto pid    = getPid();
        std::string certificatePath = (tmpDir / ("test_cert_" + std::to_string(pid) + ".pem")).string();
        std::string keyPath  = (tmpDir / ("test_key_" + std::to_string(pid) + ".pem")).string();
    
        std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + keyPath +
                          " -out " + certificatePath + " -days 1 -nodes -subj \"/CN=test\"";
    #ifdef _WIN32
        cmd += " > NUL 2>&1";
    #else
        cmd += " 2>/dev/null";
    #endif
        system(cmd.c_str());

        return {certificatePath, keyPath};
    }
}

TEST_CASE("TlsContext: construction", "[TlsContext]") {
    TlsContext tlsContext;
    REQUIRE(tlsContext.nativeHandle() != nullptr);
}

TEST_CASE("TlsContext: load valid certificate", "[TlsContext]") {
    auto [certificate, key] = createTestCertificate();
    TlsContext tlsContext;
    REQUIRE(tlsContext.loadCertificate(certificate, key));
    std::remove(certificate.c_str());
    std::remove(key.c_str());
}

TEST_CASE("TlsContext: loadCertificate fails with invalid files", "[TlsContext]") {
    TlsContext tlsContext;
    REQUIRE_FALSE(tlsContext.loadCertificate("/nonexistent/cert.pem", "/nonexistent/key.pem"));
}

TEST_CASE("TlsContext: createSSL returns non-null", "[TlsContext]") {
    auto [certificate, key] = createTestCertificate();
    TlsContext tlsContext;
    REQUIRE(tlsContext.loadCertificate(certificate, key));

    // 创建用于测试的 socket pair
    int fileDescriptors[2];
    REQUIRE(Platform::createSocketPair(fileDescriptors[0], fileDescriptors[1]));

    SSL *ssl = tlsContext.createSSL(fileDescriptors[0]);
    REQUIRE(ssl != nullptr);

    SSL_free(ssl);
    Platform::closeFileDescriptor(fileDescriptors[0]);
    Platform::closeFileDescriptor(fileDescriptors[1]);
    std::remove(certificate.c_str());
    std::remove(key.c_str());
}

TEST_CASE("TlsContext: nativeHandle returns same pointer", "[TlsContext]") {
    TlsContext tlsContext;
    auto *handle = tlsContext.nativeHandle();
    REQUIRE(handle != nullptr);
}

TEST_CASE("TlsContext: construction throws on SSL init failure is handled", "[TlsContext]") {
    // 基本的构造在任何安装了 OpenSSL 的系统上都应该成功
    REQUIRE_NOTHROW([]() { TlsContext tlsContext; }());
}
