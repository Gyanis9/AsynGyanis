/**
* @file TlsContext.h
 * @brief SSL_CTX RAII 包装器 — 管理 TLS 服务端上下文及每个连接的 SSL 对象创建
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once

#include <openssl/err.h>

#include <string>

namespace AsynGyanis::Core
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
         * @brief 构造 TlsContext 对象并创建 SSL_CTX 实例。
         *
         * @details 不做显式的库初始化：OpenSSL 1.1 起 SSL 库会自动初始化，
         *          SSL_library_init() / SSL_load_error_strings() 之类的旧接口已不再需要
         *          （也不该在库代码里替使用者调用）。创建的 SSL_CTX 使用 TLS_server_method()。
         * @throws Base::Exception SSL_CTX_new 失败
         */
        TlsContext();

        /**
         * @brief 析构函数，释放 SSL_CTX 资源（内部调用 SSL_CTX_free）。
         */
        ~TlsContext();

        TlsContext(const TlsContext &) = delete;

        TlsContext &operator=(const TlsContext &) = delete;

        /**
         * @brief 加载服务器证书和私钥文件。
         * @param certificateFile 证书文件路径（PEM 格式，通常包含证书链）
         * @param keyFile  私钥文件路径（PEM 格式）
         * @return 成功返回 true，失败返回 false（可通过 OpenSSL 错误栈获取日志）
         */
        bool loadCertificate(const std::string &certificateFile, const std::string &keyFile) const;

        /**
         * @brief 为已建立的连接创建一个新的 SSL 对象。
         * @param fileDescriptor 已连接的 socket 文件描述符（用于 SSL_set_fd 设置底层描述符）
         * @return SSL 对象指针，所有权转移给调用者（通常由 TlsSocket 持有）；
         * @throws Base::Exception SSL_new 或 SSL_set_fd 失败（本方法不返回 nullptr）
         */
        SSL *createSSL(int fileDescriptor) const;

        /**
         * @brief 获取底层的 SSL_CTX 原生句柄。
         * @return SSL_CTX* 指针
         */
        [[nodiscard]] SSL_CTX *nativeHandle() const noexcept;

    private:
        SSL_CTX *m_context{nullptr}; ///< OpenSSL SSL_CTX 句柄，RAII 管理
    };
} // namespace AsynGyanis::Core
