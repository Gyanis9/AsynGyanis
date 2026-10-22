#include "Core/Tls/TlsPeerIdentitySupport.h"

#include "Core/Crypto/Digest.h"

#include <openssl/x509.h>

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    /// X509 的 RAII 释放器：SSL_get1_peer_certificate 交回的是带引用计数的副本
    struct X509Deleter
    {
        void operator()(X509 *certificate) const noexcept
        {
            X509_free(certificate);
        }
    };

    using X509Handle = std::unique_ptr<X509, X509Deleter>;

    /// 小写十六进制：指纹是给人比对与写进配置的，大小写必须固定
    std::string toLowerHex(const std::span<const std::uint8_t> bytes)
    {
        constexpr std::array<char, 16> kHexDigits{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
        std::string                    text;
        text.reserve(bytes.size() * 2U);
        for (const std::uint8_t byte: bytes)
        {
            text.push_back(kHexDigits[byte >> 4U]);
            text.push_back(kHexDigits[byte & 0x0FU]);
        }
        return text;
    }
} // namespace

namespace AsynGyanis::Core
{
    TlsPeerIdentityPtr readTlsPeerIdentity(SSL *const session) noexcept
    {
        if (session == nullptr)
        {
            return nullptr;
        }

        try
        {
            // get1 是 OpenSSL 3 的引用计数入口；拿不到就是对端没出示证书（单向 TLS）
            const X509Handle certificate(SSL_get1_peer_certificate(session));
            if (certificate == nullptr)
            {
                return nullptr;
            }

            const int derLength = i2d_X509(certificate.get(), nullptr);
            if (derLength <= 0)
            {
                return nullptr;
            }

            std::vector<std::uint8_t> der(static_cast<std::size_t>(derLength));
            unsigned char            *writeCursor = der.data();
            if (i2d_X509(certificate.get(), &writeCursor) != derLength)
            {
                return nullptr;
            }

            const Digest::Sha256Value digest = Digest::sha256(std::string_view(reinterpret_cast<const char *>(der.data()), der.size()));
            return std::make_shared<TlsPeerIdentity>(SSL_get_verify_result(session) == X509_V_OK, std::move(der), toLowerHex(digest));
        } catch (...)
        {
            // 握手已经成功，快照读取失败不该把连接带走：业务看到空身份，按「没有证书」处理
            return nullptr;
        }
    }
} // namespace AsynGyanis::Core
