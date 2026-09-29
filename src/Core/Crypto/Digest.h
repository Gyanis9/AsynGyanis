/**
 * @file Digest.h
 * @brief 摘要与消息认证码工具：SHA-1、SHA-256、HMAC-SHA-1、HMAC-SHA-256 与十六进制文本化
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

        using Sha1Value   = std::array<std::uint8_t, kSha1Length>;   ///< SHA-1 摘要
        using Sha256Value = std::array<std::uint8_t, kSha256Length>; ///< SHA-256 摘要

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
