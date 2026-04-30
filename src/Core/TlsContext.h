/**
* @file TlsContext.h
 * @brief SSL_CTX RAII 包装器 — 管理 TLS 服务端上下文及每个连接的 SSL 对象创建
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_TLSCONTEXT_H
#define CORE_TLSCONTEXT_H

#include <openssl/err.h>

#include <string>

namespace Core
{
    /**
     * @brief SSL_CTX 的 RAII 包装器，管理 TLS 服务端上下文和每个连接的 SSL 对象创建。
     *
     * 封装 OpenSSL SSL_CTX 的生命周期（创建、配置、加载证书/私钥），
     * 并提供创建 SSL 对象的方法。析构时自动释放 SSL_CTX 资源。
     * 适用于服务端 TLS 连接的上下文管理。
     */
    class TlsContext
    {
    public:
        /**
         * @brief 构造 TlsContext 对象，初始化 OpenSSL 库并创建 SSL_CTX 实例。
         *
         * 内部调用 SSL_library_init()、OpenSSL_add_all_algorithms()、
         * SSL_load_error_strings() 等初始化函数（通常只应调用一次，但多次调用也无妨）。
         * 创建使用 TLS_server_method() 的 SSL_CTX。
         */
        TlsContext();

        /**
         * @brief 析构函数，释放 SSL_CTX 资源（内部调用 SSL_CTX_free）。
         */
        ~TlsContext();

        TlsContext(const TlsContext &)            = delete;
        TlsContext &operator=(const TlsContext &) = delete;

        /**
         * @brief 加载服务器证书和私钥文件。
         * @param certFile 证书文件路径（PEM 格式，通常包含证书链）
         * @param keyFile  私钥文件路径（PEM 格式）
         * @return 成功返回 true，失败返回 false（可通过 OpenSSL 错误栈获取日志）
         */
        bool loadCertificate(const std::string &certFile, const std::string &keyFile) const;

        /**
         * @brief 为已建立的连接创建一个新的 SSL 对象。
         * @param fd 已连接的 socket 文件描述符（用于设置底层 fd）
         * @return SSL 对象指针，所有权转移给调用者（通常由 TlsSocket 持有）；
         *         失败返回 nullptr。
         */
        SSL *createSSL(int fd) const;

        /**
         * @brief 获取底层的 SSL_CTX 原生句柄。
         * @return SSL_CTX* 指针
         */
        [[nodiscard]] SSL_CTX *nativeHandle() const noexcept;

    private:
        SSL_CTX *m_ctx{nullptr}; ///< OpenSSL SSL_CTX 句柄，RAII 管理
    };
}

#endif
