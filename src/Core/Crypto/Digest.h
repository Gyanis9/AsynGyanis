/**
 * @file Digest.h
 * @brief 摘要与消息认证码工具：SHA-1/256/384/512、HMAC-SHA-1/256、十六进制文本化，以及算法选择子到 OpenSSL 名字的映射
 * @author Gyanis
 * @date 2026-09-27
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace AsynGyanis::Core
{
    /**
     * @brief 摘要与 HMAC 工具（OpenSSL EVP 封装）
     *
     * @details 放在 Core 而不是 Base：Base 不链接 OpenSSL，而本层的存在理由就是把 EVP 的
     *          「建上下文 → 喂数据 → 收尾 → 释放」这段必须一次做对、漏一步就是句柄泄漏的样板
     *          收在一处。签名令牌、ETag、WebSocket 握手算 accept 值都用它。
     * @note SHA-1 一族只服务两个**规定**了它的场景：RFC 6455 的握手摘要与外部服务规定的
     *       HMAC-SHA1 请求签名（如阿里云 RPC 风格 OpenAPI）。不要把它当抗碰撞哈希或通用
     *       消息认证码用，需要完整性强度时用 SHA-256 一族
     */
    namespace Digest
    {
        /// SHA-1 摘要长度，单位字节
        inline constexpr std::size_t kSha1Length = 20;

        /// SHA-256 摘要长度，单位字节
        inline constexpr std::size_t kSha256Length = 32;

        /// SHA-384 摘要长度，单位字节
        inline constexpr std::size_t kSha384Length = 48;

        /// SHA-512 摘要长度，单位字节
        inline constexpr std::size_t kSha512Length = 64;

        using Sha1Value   = std::array<std::uint8_t, kSha1Length>;   ///< SHA-1 摘要
        using Sha256Value = std::array<std::uint8_t, kSha256Length>; ///< SHA-256 摘要
        using Sha384Value = std::array<std::uint8_t, kSha384Length>; ///< SHA-384 摘要
        using Sha512Value = std::array<std::uint8_t, kSha512Length>; ///< SHA-512 摘要

        /**
         * @brief 摘要算法的选择子
         * @details 存在的理由是把「算法 → OpenSSL 的摘要名」这段映射收在一处：密钥套表与 KDF 这些
         *          按套件挑哈希的地方原先各写一份字符串（`"SHA2-256"` 之类），拼错一处就是
         *          悄悄换了个哈希，而这里换成枚举后编译器管得住。
         */
        enum class Algorithm
        {
            Sha256, ///< SHA-256（32 字节摘要，QUIC 的 AES-128-GCM 套件用它）
            Sha384, ///< SHA-384（48 字节摘要，QUIC 的 AES-256-GCM 套件用它）
            Sha512, ///< SHA-512（64 字节摘要）
        };

        /**
         * @brief 取某个算法在 OpenSSL 里的摘要名
         * @details 名字取 `OSSL_DIGEST_NAME_*` 那一族而不是硬写字符串：小版本之间改过写法，
         *          用宏才跟着头文件走
         * @param algorithm 目标算法
         * @return std::string_view 静态长度的摘要名，调用方可直接拷进参数缓冲
         */
        [[nodiscard]] ASYN_CORE_API std::string_view openSslName(Algorithm algorithm);

        /**
         * @brief 取某个算法的摘要长度，单位字节
         * @details KDF 一族靠它定两件事：HKDF 的 PRK 长度（RFC 5869 §2.2 就是哈希长度）与
         *          输出上限（§2.3 的 255 × Hash.length）
         * @param algorithm 目标算法
         * @return std::size_t SHA-256 → 32、SHA-384 → 48、SHA-512 → 64
         */
        [[nodiscard]] ASYN_CORE_API std::size_t byteLength(Algorithm algorithm);

        /**
         * @brief 算 SHA-1 摘要
         * @details 走 EVP_sha1()：OpenSSL 3.0 起 `SHA1()` 便捷函数被标记废弃，直接用会吃到弃用告警
         * @param data 待摘要数据（按「指针 + 长度」取，允许含 NUL）
         * @return Sha1Value 20 字节摘要
         * @throws Base::Exception 上下文创建失败或摘要接口未能算出完整结果
         */
        [[nodiscard]] ASYN_CORE_API Sha1Value sha1(std::string_view data);

        /**
         * @brief 算 HMAC-SHA-1
         * @details 存在的唯一理由是 RFC 2104 那一族的**消息认证码**形态：部分外部服务的请求签名
         *          （如阿里云 RPC 风格的 OpenAPI）规定用 HMAC-SHA1，拿裸 SHA-1 或 SHA-256 换不得。
         *          与 sha1() 同一条告警：这不是抗碰撞哈希，只用于带密钥的认证。
         * @param key 密钥，按「指针 + 长度」取，允许任意字节（含 NUL）
         * @param data 待认证数据
         * @return Sha1Value 20 字节认证码
         * @throws Base::Exception HMAC 上下文创建失败或接口未能算出完整结果
         */
        [[nodiscard]] ASYN_CORE_API Sha1Value hmacSha1(std::string_view key, std::string_view data);

        /**
         * @brief 算 SHA-256 摘要
         * @param data 待摘要数据（允许含 NUL）
         * @return Sha256Value 32 字节摘要
         * @throws Base::Exception 上下文创建失败或摘要接口未能算出完整结果
         */
        [[nodiscard]] ASYN_CORE_API Sha256Value sha256(std::string_view data);

        /**
         * @brief 算 SHA-384 摘要
         * @param data 待摘要数据（允许含 NUL）
         * @return Sha384Value 48 字节摘要
         * @throws Base::Exception 上下文创建失败或摘要接口未能算出完整结果
         */
        [[nodiscard]] ASYN_CORE_API Sha384Value sha384(std::string_view data);

        /**
         * @brief 算 SHA-512 摘要
         * @param data 待摘要数据（允许含 NUL）
         * @return Sha512Value 64 字节摘要
         * @throws Base::Exception 上下文创建失败或摘要接口未能算出完整结果
         */
        [[nodiscard]] ASYN_CORE_API Sha512Value sha512(std::string_view data);

        /**
         * @brief 算 HMAC-SHA-256
         * @details 密钥按「指针 + 长度」取，允许任意字节（含 NUL）：令牌签名常见的错法是把密钥
         *          当零终止字符串处理，长度就被截断了
         * @param key 密钥
         * @param data 待认证数据
         * @return Sha256Value 32 字节认证码
         * @throws Base::Exception HMAC 上下文创建失败或接口未能算出完整结果
         */
        [[nodiscard]] ASYN_CORE_API Sha256Value hmacSha256(std::string_view key, std::string_view data);

        /**
         * @brief 把一段字节转成小写十六进制文本
         * @param bytes 待转换字节
         * @return std::string 长度是输入两倍的十六进制文本
         */
        [[nodiscard]] ASYN_CORE_API std::string toHex(std::span<const std::uint8_t> bytes);

        /**
         * @brief SHA-256 摘要并直接落成十六进制文本（ETag、凭据指纹常用的形状）
         * @param data 待摘要数据
         * @return std::string 64 字符的十六进制文本
         * @throws Base::Exception 同 sha256()
         */
        [[nodiscard]] ASYN_CORE_API std::string sha256Hex(std::string_view data);

        /**
         * @brief HMAC-SHA-256 并直接落成十六进制文本
         * @param key 密钥
         * @param data 待认证数据
         * @return std::string 64 字符的十六进制文本
         * @throws Base::Exception 同 hmacSha256()
         */
        [[nodiscard]] ASYN_CORE_API std::string hmacSha256Hex(std::string_view key, std::string_view data);
    } // namespace Digest
} // namespace AsynGyanis::Core
