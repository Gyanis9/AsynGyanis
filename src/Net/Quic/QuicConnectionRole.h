/**
 * @file QuicConnectionRole.h
 * @brief 本端在一条 QUIC 连接里的角色：传输核心与流层共用的那一份判据
 * @author Gyanis
 * @date 2026-09-26
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 单独一个头是因为两个使用方隔着包含方向：`QuicConnectionCore` 含流层，角色枚举若写在任何
 *          一边的头里，另一边要么反向包含（成环）要么各自抄一份（会漂）。
 */

#pragma once

#include <cstdint>

namespace AsynGyanis::Net
{
    /**
     * @brief 本端角色，决定四类事：Initial 密钥的收发方向、TLS 侧以谁的身份建、传输参数里那几个
     *        「仅服务端」项与 §7.3 的绑定校验查哪一边、以及只约束服务端的那几条规则（地址验证与
     *        反放大、HANDSHAKE_DONE 的方向、流号低位）
     */
    enum class QuicConnectionRole : std::uint8_t
    {
        Server, ///< 服务端：等客户端先出声，且受 §8.1 的反放大上限约束
        Client, ///< 客户端：主动发第一个 Initial，并按 §14.1 把它补到最小尺寸
    };
} // namespace AsynGyanis::Net
