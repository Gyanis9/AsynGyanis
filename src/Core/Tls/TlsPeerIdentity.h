/**
 * @file TlsPeerIdentity.h
 * @brief TLS 对端证书的身份快照：验证结论、叶证书 DER 与 SHA-256 指纹
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace AsynGyanis::Core
{
    /**
     * @brief 一次 TLS 握手中对端出示的叶证书身份
     *
     * @details 值语义、不可变：会话在握手完成后抓取一份，之后每条请求共享同一个 shared_ptr。
     *          不暴露裸 `X509*`：那张指针归 SSL 会话所有、只在 TlsSocket 的线程上安全，
     *          业务拿到它迟早会跨线程或越过握手期使用。这里只交出 DER 与指纹，调用方可自由复制。
     * @note `isVerified()` 取自 OpenSSL 的 `SSL_get_verify_result`：为假表示对端出示了证书但链/用途
     *       没通过校验（例如 mTLS 设为可选）。要求客户端证书的服务器握手阶段就会拒绝这种连接，
     *       因此业务在 requireClientCertificates 打开时看到的一定是已验证的身份。
     */
    class ASYN_CORE_API TlsPeerIdentity final
    {
    public:
        /**
         * @brief 构造身份快照
         * @param isVerified 握手时的证书链校验结论
         * @param certificateDer 叶证书的 DER 编码
         * @param sha256FingerprintHex 叶证书 DER 的 SHA-256，小写十六进制（64 字符）
         */
        TlsPeerIdentity(const bool isVerified, std::vector<std::uint8_t> certificateDer, std::string sha256FingerprintHex) :
            m_isVerified(isVerified), m_certificateDer(std::move(certificateDer)), m_sha256FingerprintHex(std::move(sha256FingerprintHex))
        {
        }

        /**
         * @brief 证书链校验是否通过
         * @return true 已验证；false 对端出示了证书但校验未通过
         */
        [[nodiscard]] bool isVerified() const noexcept
        {
            return m_isVerified;
        }

        /**
         * @brief 叶证书的 DER 编码
         * @return const std::vector<std::uint8_t>& 证书原始字节，生命周期随本对象
         */
        [[nodiscard]] const std::vector<std::uint8_t> &certificateDer() const noexcept
        {
            return m_certificateDer;
        }

        /**
         * @brief 叶证书 DER 的 SHA-256 指纹（小写十六进制）
         * @return const std::string& 64 个字符；调用方可直接做相等比较
         */
        [[nodiscard]] const std::string &sha256FingerprintHex() const noexcept
        {
            return m_sha256FingerprintHex;
        }

    private:
        bool                      m_isVerified;           ///< 握手时的链校验结论
        std::vector<std::uint8_t> m_certificateDer;       ///< 叶证书 DER
        std::string               m_sha256FingerprintHex; ///< 叶证书 DER 的 SHA-256，小写十六进制
    };

    /// 对端身份快照的共享句柄：会话抓取一次，多条请求共享，复制不搬 DER
    using TlsPeerIdentityPtr = std::shared_ptr<const TlsPeerIdentity>;
} // namespace AsynGyanis::Core
