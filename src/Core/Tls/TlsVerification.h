/**
 * @file TlsVerification.h
 * @brief 客户端对服务端装订的 OCSP 响应做语义校验
 * @author Gyanis
 * @date 2026-10-27
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "AsynGyanisExport.h"

#include <openssl/types.h>

namespace AsynGyanis::Core
{
    /**
     * @brief 校验已完成握手的客户端会话收到的 OCSP 装订响应
     * @details 只有收到响应才校验：未装订时直接返回，保持「服务端不提供 OCSP 也能连接」的既有行为。
     *          校验覆盖 DER 编码、响应状态、已验证证书链上的签名、CertID 匹配（按响应声明的
     *          摘要算法）、有效期与 GOOD 状态。
     * @param ssl 已完成 SSL_connect 的客户端会话；空指针按失败处理
     * @throws CoreException 收到响应但任一项校验不通过（含状态为已吊销）；失败即拒握手
     * @note 必须在对端证书校验已开启（SSL_VERIFY_PEER）的会话上调用：签名校验要用到
     *       SSL_get0_verified_chain 的叶证书与签发者，未开校验时判为失败而不是跳过
     */
    ASYN_CORE_API void verifyStapledOcspResponse(SSL *ssl);
} // namespace AsynGyanis::Core
