/**
 * @file QuicCipherContext.h
 * @brief 每线程复用的 OpenSSL 密码上下文：把「每包 new/free 一遍」换成「每线程一次」
 * @author Gyanis
 * @date 2026-09-23
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 一条出站包要过 AEAD 与头部保护各一次，入站包同量。上下文本身与密钥无关，重置后
 *          换一组密钥与 IV 就能继续用，因此按线程持有；剩下每次运算必须做的只有
 *          「设密码 + 交密钥 + 交 IV」这三步。
 */

#pragma once

#include <openssl/evp.h>

namespace AsynGyanis::Net
{
    /**
     * @brief 取本线程复用的密码上下文，取到即已重置为干净状态
     * @details 返回值由本模块持有：调用方不得 free，也不需要自己 new——下一次取用会先重置。
     *          AEAD 校验失败的那次运算会把上下文停在半程，「取用即重置」正是为了不把那份状态
     *          留给下一个包。上下文只在所在线程内使用，与「事件循环对象不跨线程碰」的契约一致。
     * @return EVP_CIPHER_CTX * 本线程的上下文；OpenSSL 创建失败时为 nullptr，由调用方按运行期故障处理
     */
    [[nodiscard]] EVP_CIPHER_CTX *acquireQuicCipherContext() noexcept;
} // namespace AsynGyanis::Net
