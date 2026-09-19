/**
 * @file QuicRawBytes.h
 * @brief QUIC 字节缓冲的原样追写：把 `std::span<const std::uint8_t>` 写进二进制安全的 `std::string`
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 组包侧的每个字段最终都要落到同一个 `std::string` 里（报文与帧都是二进制），
 *          这个「空视图不能把 data() 交给 append」的坑由本函数一次性挡掉，各编码文件不再各写一份。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace AsynGyanis::Net
{
    /**
     * @brief 把一段字节原样追写到缓冲末尾
     * @param bytes 目标缓冲，二进制安全
     * @param data 待写入字节；空视图直接返回，因为空 vector 的 data() 允许是空指针，
     *        而 `append(nullptr, 0)` 不在标准保证的范围内
     */
    inline void appendQuicRawBytes(std::string &bytes, const std::span<const std::uint8_t> data)
    {
        if (data.empty())
        {
            return;
        }
        bytes.append(reinterpret_cast<const char *>(data.data()), data.size());
    }
} // namespace AsynGyanis::Net
