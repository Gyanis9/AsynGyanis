/**
 * @file QuicSupport.h
 * @brief QUIC 内部共用的小工具：饱和减法与随机字节
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "Base/Exception/Exception.h"
#include "Core/Crypto/Random.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace AsynGyanis::Net::QuicDetail
{
    /**
     * @brief 不会回绕的减法
     * @param total 被减数
     * @param part 减数
     * @return std::size_t 差值；part 大于 total 时为 0
     */
    [[nodiscard]] inline std::size_t saturatingSubtract(const std::size_t total, const std::size_t part) noexcept
    {
        return part > total ? 0 : total - part;
    }

    /**
     * @brief 取一段密码学随机字节
     * @param byteLength 需要的字节数
     * @return std::vector<std::uint8_t> 成功时的字节；随机源不可用时空 vector
     */
    [[nodiscard]] inline std::vector<std::uint8_t> randomBytes(const std::size_t byteLength)
    {
        try
        {
            return Core::Random::bytes(byteLength);
        } catch (const Base::Exception &)
        {
            return {};
        }
    }
} // namespace AsynGyanis::Net::QuicDetail
