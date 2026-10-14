/**
 * @file QuicAddressValidationToken.h
 * @brief QUIC 地址验证令牌的签发与核验：把对端地址、原始目的标识与签发时刻封进一份带 HMAC 的无状态令牌
 * @author Gyanis
 * @date 2026-10-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/Crypto/Digest.h"
#include "Platform/IO/Socket.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief 令牌核验没能通过的原因
     *
     * @details 服务端按这一格决定下一步：Absent 是「还没验过」，回一条 Retry；IntegrityFailed /
     *          Malformed / AddressMismatch / Expired 都是「验不过」，按 RFC 9000 §8.1.2 只能拒或放行，
     *          不能再回 Retry。把这几格分开是为了让读数说得清失败形态——合成一格就等于只知道「不给建」。
     */
    enum class QuicAddressValidationTokenStatus
    {
        Valid,           ///< 令牌由本端签发、未过期、且打到的地址与签发时一致
        Absent,          ///< 报文里根本没有令牌：这是「还没验过地址」，不是伪造
        Malformed,       ///< 长度或字段不合本端的令牌形状（含不是本端这一类的令牌）
        IntegrityFailed, ///< 认证码不合：被改过、或凭空造的
        AddressMismatch, ///< 认证码合但地址不合：令牌是从别处搬来的
        Expired,         ///< 已过期，或签发时刻在未来（时钟不合）
    };

    /**
     * @brief 核验通过的令牌里恢复出来的信息
     *
     * @details 原始目的标识是「参数里的 original_destination_connection_id」与「Initial 密钥的算据」的来源，
     *          服务端必须拿它而不是报文里那个已被 Retry 换掉的标识；签发时刻只用于日志与观测。
     */
    struct QuicAddressValidationTokenContent
    {
        std::vector<std::uint8_t> originalDestinationConnectionId{}; ///< 客户端**第一个** Initial 的目的标识
        std::chrono::seconds      issuedAge{0};                      ///< 距今多久前签发（核验时算出）
    };

    /**
     * @brief 无状态地址验证令牌的签发器与核验器（RFC 9000 §8.1.2、§8.1.4）
     *
     * @details 一份本端密钥就能把「谁、在哪个地址、什么时候」封进令牌，服务端不为此留任何连接状态——
     *          这正是 Retry 存在的理由：握手的状态与密码学开销都推迟到对端证明它真能收到回包之后。
     *          令牌布局是 `类别(1) || 签发秒(4) || 原始目的标识长度(1) || 原始目的标识 || 地址栏(16) || 认证码(16)`；
     *          前缀明文、后两段由密钥算出，所以令牌本身可被日志读出长度却不仿造得出。
     * @warning 多进程 worker 必须共享同一份密钥：各进程随机自造时，客户端带着 A 进程发的令牌撞到
     *          B 进程必然验不过（同一份票据密钥要共享的道理见 `QuicServer::Configuration::sessionTicketKeyFiles`）。
     */
    class ASYN_NET_API QuicAddressValidationTokenCodec
    {
    public:
        /**
         * @brief 用给定密钥建一个签发器
         * @param signingKey 本端密钥；至少 16 字节，短于此刻构造当场抛
         * @param lifetime 令牌有效期；必须为正数——RFC 9000 §8.1.4 要求 Retry 令牌「只在很短时间内被接受」，
         *        给不出有限期就等于不设这条防线
         * @throws Base::InvalidArgumentException 用法错误：密钥太短、或有效期不是正数
         */
        QuicAddressValidationTokenCodec(std::span<const std::uint8_t> signingKey, std::chrono::seconds lifetime);

        /**
         * @brief 建一个密钥随机自造的签发器（没配密钥时的出路）
         * @details 随机源坏了就交回空指针：地址验证没有安全的降级路径，拿全零密钥签出的令牌等于不发。
         * @param lifetime 令牌有效期
         * @return 有值＝可用的签发器；空＝随机源不可用
         */
        [[nodiscard]] static std::unique_ptr<QuicAddressValidationTokenCodec> createWithRandomKey(std::chrono::seconds lifetime);

        /**
         * @brief 换一把新密钥：新令牌按新密钥签发，旧密钥留着验它签过的那批
         *
         * @details 轮换窗口就是令牌的有效期本身——旧密钥签出的令牌最长活到这里，过期之后无需再认它。
         *          留着旧密钥而不是立刻丢弃，是因为正在握手的客户端手里那份令牌此刻还得能用；丢了它，
         *          一次轮换就把一批好连接按「令牌不合」掐死。
         * @param newSigningKey 新密钥，长度要求与构造时一致
         * @throws Base::InvalidArgumentException 用法错误：新密钥太短
         */
        void rotateSigningKey(std::span<const std::uint8_t> newSigningKey);

        /**
         * @brief 给这个地址签一张令牌
         * @param peerAddress 客户端报文的来源地址（IP 与端口一起封进去，RFC 9000 §8.1.4 的 SHOULD）
         * @param originalDestinationConnectionId 客户端第一个 Initial 的目的标识，核验时还回去
         * @param issuedAt 签发时刻（由调用方给出，使这条换算能被确定性地测出来）
         * @return 令牌字节；随机源或 HMAC 坏了时为空——调用方据此放弃回 Retry，而不是发一张没认证的令牌
         */
        [[nodiscard]] std::vector<std::uint8_t> issue(const Platform::SocketAddress &peerAddress, std::span<const std::uint8_t> originalDestinationConnectionId,
                                                      std::chrono::system_clock::time_point issuedAt) const;

        /**
         * @brief 核验一张令牌，并在通过时把封进去的信息还回来
         * @details 认证码用常数时间比对：这道判定是「这张令牌是不是本端给这个地址签的」，比错的耗时差异
         *          会把密钥位泄露出去（RFC 9000 §10.3.1 对无状态重置令牌提的是同一条要求）。
         * @param token 报文里的令牌字段（可为空——空即 `Absent`，不是失败）
         * @param peerAddress 现在这个报文的来源地址
         * @param now 核验时刻（由调用方给出）
         * @return 通过返回封进去的内容
         * @return 失败返回具体的不合原因
         */
        [[nodiscard]] std::expected<QuicAddressValidationTokenContent, QuicAddressValidationTokenStatus>
        verify(std::span<const std::uint8_t> token, const Platform::SocketAddress &peerAddress, std::chrono::system_clock::time_point now) const;

        /// 令牌有效期（读数与日志要用，别让配置在服务器里再抄一份）
        [[nodiscard]] std::chrono::seconds lifetime() const noexcept;

    private:
        /**
         * @brief 一把密钥算出的两个子密钥（地址栏与认证码各用一个）
         * @details 域分离：同一把密钥直接算两处 HMAC，两处的输出就能互相凑——拆成两个派生标签之后，
         *          拿到一类的输出也推不出另一类。
         */
        struct KeySet
        {
            std::array<std::uint8_t, Core::Digest::kSha256Length> addressKey{}; ///< 算地址栏用
            std::array<std::uint8_t, Core::Digest::kSha256Length> authKey{};    ///< 算认证码用
        };

        [[nodiscard]] static KeySet deriveKeySet(std::span<const std::uint8_t> signingKey);

        std::vector<KeySet>  m_keySets{};                         ///< 第一把是当前密钥，其余是轮换窗口内的旧密钥
        std::chrono::seconds m_lifetime{std::chrono::seconds{0}}; ///< 有效期，构造时校验为正
    };
} // namespace AsynGyanis::Net
