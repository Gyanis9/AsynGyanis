/**
 * @file TlsContext.h
 * @brief SSL_CTX RAII wrapper — manages TLS server context and per-connection SSL creation
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_TLSCONTEXT_H
#define CORE_TLSCONTEXT_H

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <string>

namespace Core
{
    class TlsContext
    {
    public:
        TlsContext();

        ~TlsContext();

        TlsContext(const TlsContext &)            = delete;
        TlsContext &operator=(const TlsContext &) = delete;

        /**
         * @brief Load server certificate and private key.
         * @return true on success
         */
        bool loadCertificate(const std::string &certFile, const std::string &keyFile);

        /**
         * @brief Create a new SSL object for an accepted connection.
         * @param fd the connected socket file descriptor
         * @return SSL* (non-owning; ownership transfers to TlsSocket)
         */
        SSL *createSSL(int fd);

        [[nodiscard]] SSL_CTX *nativeHandle() const noexcept;

    private:
        SSL_CTX *m_ctx{nullptr};
    };

}

#endif
