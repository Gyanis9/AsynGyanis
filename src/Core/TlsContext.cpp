#include "TlsContext.h"

#include "Base/Exception.h"

namespace Core
{
    TlsContext::TlsContext()
    {
        m_ctx = SSL_CTX_new(TLS_server_method());
        if (!m_ctx)
        {
            throw Base::Exception("TlsContext: SSL_CTX_new failed");
        }

        SSL_CTX_set_options(m_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
        SSL_CTX_set_mode(m_ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    }

    TlsContext::~TlsContext()
    {
        if (m_ctx)
        {
            SSL_CTX_free(m_ctx);
            m_ctx = nullptr;
        }
    }

    bool TlsContext::loadCertificate(const std::string &certFile, const std::string &keyFile)
    {
        if (SSL_CTX_use_certificate_file(m_ctx, certFile.c_str(), SSL_FILETYPE_PEM) != 1)
        {
            return false;
        }
        if (SSL_CTX_use_PrivateKey_file(m_ctx, keyFile.c_str(), SSL_FILETYPE_PEM) != 1)
        {
            return false;
        }
        if (SSL_CTX_check_private_key(m_ctx) != 1)
        {
            return false;
        }
        return true;
    }

    SSL *TlsContext::createSSL(const int fd)
    {
        SSL *ssl = SSL_new(m_ctx);
        if (!ssl)
        {
            throw Base::Exception("TlsContext: SSL_new failed");
        }
        SSL_set_fd(ssl, fd);
        return ssl;
    }

    SSL_CTX *TlsContext::nativeHandle() const noexcept
    {
        return m_ctx;
    }

}
