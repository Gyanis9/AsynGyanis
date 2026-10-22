/**
 * @file JwtVerifier.h
 * @brief JWT（RS256）的解析、验签与声明校验 —— 纯函数层，给鉴权件做「按公钥验签」的底座
 * @author Gyanis
 * @date 2026-10-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details OAuth 侧的令牌大多是 RS256 签名的 JWT：验它需要「解析三段 → 取公钥 → 验签 →
 *          校验声明」四步，每一步都有各自的拒绝面（alg 混淆、篡改、过期、iss/aud 不符）。
 *          本文件把这四步做成无状态纯函数：不拉网络、不持有缓存、时钟由调用方传入——
 *          因此可以在用例里对着现场签好的令牌逐格证伪。公钥的来源（JWKS 端点、缓存与轮换）
 *          是另一半，建在本层之上而不是塞进来。
 *
 * @note 拒绝一律「点名原因」（中文），且先验签后读载荷：未验签的载荷不可信，把
 *       「解析出来给调用方看一眼」做成默认行为等于把不可信数据递出去。要读未验签的载荷
 *       （例如只为取 kid 选键）请显式走 parseJwt()，名字里的「解析」就是提醒。
 * @warning 只认 RS256。`alg: none` 与 HMAC（HS256）都必须被拒——前者是「无签名」的公开
 *          伪装，后者的「公钥当 HMAC 密钥用」是经典混淆攻击；本层把 alg 白名单钉死在
 *          验签入口（verifyJwtRs256），不要在外层自己放宽。
 */
#pragma once

#include "AsynGyanisExport.h"

#include <openssl/evp.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    /**
     * @brief base64url（RFC 4648 §5，无填充）编码
     * @param bytes 原始字节
     * @return std::string 编码文本（不含 '=' 填充）
     */
    [[nodiscard]] ASYN_NET_API std::string base64UrlEncode(std::string_view bytes);

    /**
     * @brief base64url（RFC 4648 §5，无填充）解码
     * @details JWT 的三段按规范不带填充；出现 '+' '/' '=' 或非法字符一律给空值——
     *          宽容解析会把畸形令牌放进来，而拒绝面正是本层的价值。
     * @param text 编码文本
     * @return std::optional<std::string> 解码字节；非法输入给空
     */
    [[nodiscard]] ASYN_NET_API std::optional<std::string> base64UrlDecode(std::string_view text);

    /**
     * @brief 解析后的 JWT 三段（未验签）
     */
    struct ParsedJwt
    {
        std::string headerJson;   ///< 头段解出的 JSON 文本
        std::string payloadJson;  ///< 载荷段解出的 JSON 文本
        std::string signature;    ///< 签名原始字节（第三段 base64url 解码后）
        std::string signingInput; ///< "header.payload" 的原文（验签输入必须用原文而不是重编码）
    };

    /**
     * @brief 拆开 JWT 并解码前两段（只做形状与编码检查，不验签）
     * @param token 完整令牌（三段、点分隔）
     * @return std::optional<ParsedJwt> 解析结果；段数不对、base64url 非法或头/载荷不是 JSON 时给空
     */
    [[nodiscard]] ASYN_NET_API std::optional<ParsedJwt> parseJwt(std::string_view token);

    /**
     * @brief 从 RSA JWK 的 n/e 构造公钥
     * @details n 与 e 都是 base64url（无填充）的大端字节；空串、非 base64url、或 n/e 长度为 0 时给空。
     *          返回的 shared_ptr 在最后一个引用消失时释放（EVP_PKEY_free），调用方可以直接长期持有。
     * @param modulusN 模数 n 的 base64url 文本
     * @param exponentE 指数 e 的 base64url 文本
     * @return std::shared_ptr<EVP_PKEY> 公钥；构造失败给空
     */
    [[nodiscard]] ASYN_NET_API std::shared_ptr<EVP_PKEY> rsaPublicKeyFromJwk(std::string_view modulusN, std::string_view exponentE);

    /**
     * @brief RS256 验签（RSASSA-PKCS1-v1_5 + SHA-256）
     * @details 验的是 ParsedJwt::signingInput 的原文与第三段解码后的签名，公钥按调用方给的来。
     *          本函数不看 header.alg——「令牌自称什么算法」不是判据，判据在 verifyJwtRs256() 的白名单里。
     * @param parsed parseJwt() 的输出
     * @param publicKey 公钥；空指针给 false
     * @return true 签名有效
     */
    [[nodiscard]] ASYN_NET_API bool verifyRs256(const ParsedJwt &parsed, EVP_PKEY *publicKey);

    /**
     * @brief 声明校验的期望值（都为空时只校时间）
     */
    struct JwtClaimExpectations
    {
        std::string          issuer;        ///< 期望的 iss（RFC 7519 §4.1.1）；空 = 不校验
        std::string          audience;      ///< 期望的 aud：载荷里是字符串时精确相等、是数组时含于其中即命中；空 = 不校验
        std::chrono::seconds clockSkew{60}; ///< 容许偏差（秒）：负值按 0、超过一年按一年钳制；exp / nbf 两侧都用这个值
    };

    /**
     * @brief 校验 exp / nbf / iss / aud
     * @details 时间声明按 RFC 7519 §4.1.4 / §4.1.5 解读为 NumericDate（秒）；未给出 exp 或 nbf 时
     *          该项不判（规范允许缺省），但 iss / aud 一旦在期望里给出就必须命中。时钟由调用方传入，
     *          本层不读系统时间——用例因此可以对「过期前一秒、过期后一秒」逐点证伪。
     * @param payloadJson 载荷段的 JSON 文本
     * @param expectations 期望值
     * @param now 当前时刻
     * @return std::optional<std::string> 通过为空；不通过给出中文原因
     */
    [[nodiscard]] ASYN_NET_API std::optional<std::string> validateJwtClaims(std::string_view payloadJson, const JwtClaimExpectations &expectations,
                                                                            std::chrono::system_clock::time_point now);

    /**
     * @brief 一步到位：解析 → alg 白名单（只认 RS256）→ 验签 → 声明校验
     * @param token 完整令牌
     * @param publicKey 公钥
     * @param expectations 声明期望
     * @param now 当前时刻
     * @return std::optional<std::string> 通过为空；不通过给出中文原因（可直接进日志或回给调用方）
     * @note 任何一步失败都只给原因、不抛异常：验签属于「对端输入驱动」的路径，抛异常会让每条
     *       畸形令牌都变成一次异常分发
     */
    [[nodiscard]] ASYN_NET_API std::optional<std::string> verifyJwtRs256(std::string_view token, EVP_PKEY *publicKey, const JwtClaimExpectations &expectations,
                                                                         std::chrono::system_clock::time_point now);

} // namespace AsynGyanis::Net
