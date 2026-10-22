/**
 * @file TlsPeerIdentitySupport.h
 * @brief TLS 对端证书快照的内部读取器（不安装）
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Tls/TlsPeerIdentity.h"

#include <openssl/ssl.h>

namespace AsynGyanis::Core
{
    /**
     * @brief 从已完成握手的 SSL 会话读取对端叶证书身份
     * @details TlsSocket 与 QUIC 的 QuicTlsContext 共用这一份转换：DER 编码、SHA-256 指纹与
     *          链校验结论只有一处实现，两个传输层不会在指纹口径上分叉。
     * @param session 已完成握手的 SSL 会话；空指针返回空
     * @return 身份快照；对端没有出示证书（单向 TLS）时为空
     */
    [[nodiscard]] TlsPeerIdentityPtr readTlsPeerIdentity(SSL *session) noexcept;
} // namespace AsynGyanis::Core
