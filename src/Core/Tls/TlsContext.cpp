#include "Core/Tls/TlsContext.h"
#include "Base/Exception/SystemException.h"

#include <openssl/ssl.h>

namespace AsynGyanis::Core
{
    TlsContext::TlsContext()
    {
        m_context = SSL_CTX_new(TLS_server_method());
        if (!m_context)
        {
            throw Base::Exception("TlsContext: SSL_CTX_new failed");
        }

        SSL_CTX_set_options(m_context, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
        // 强制最低协议为 TLS 1.2，禁用已被废弃且不安全的 TLS 1.0/1.1
        SSL_CTX_set_min_proto_version(m_context, TLS1_2_VERSION);
        SSL_CTX_set_mode(m_context, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    }

    TlsContext::~TlsContext()
    {
        if (m_context)
        {
            SSL_CTX_free(m_context);
            m_context = nullptr;
        }
    }

    bool TlsContext::loadCertificate(const std::string &certificateFile, const std::string &keyFile) const
    {
        if (SSL_CTX_use_certificate_file(m_context, certificateFile.c_str(), SSL_FILETYPE_PEM) != 1)
        {
            return false;
        }

        if (SSL_CTX_use_PrivateKey_file(m_context, keyFile.c_str(), SSL_FILETYPE_PEM) != 1)
        {
            return false;
        }

        if (SSL_CTX_check_private_key(m_context) != 1)
        {
            return false;
        }
        return true;
    }

    SSL *TlsContext::createSSL(const int fileDescriptor) const
    {
        SSL *ssl = SSL_new(m_context);
        if (!ssl)
        {
            throw Base::Exception("TlsContext: SSL_new failed");
        }
        if (SSL_set_fd(ssl, fileDescriptor) == 0)
        {
            SSL_free(ssl);
            throw Base::Exception("TlsContext: SSL_set_fd failed");
        }
        return ssl;
    }

    SSL_CTX *TlsContext::nativeHandle() const noexcept
    {
        return m_context;
    }

}
