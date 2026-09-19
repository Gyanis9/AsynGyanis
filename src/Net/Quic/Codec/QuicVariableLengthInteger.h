/**
 * @file QuicVariableLengthInteger.h
 * @brief QUIC 变长整数（RFC 9000 §16）的编码与解码：1/2/4/8 字节四档，最大 2^62-1
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/InvalidArgumentException.h"
#include "Net/Quic/Codec/QuicDecodeError.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace AsynGyanis::Net
{
    /// 8 字节档可用的位数是 62，因此这是全协议里所有「用变长整数表示的值」的共同上限（RFC 9000 §16 表 4）
    inline constexpr std::uint64_t kQuicMaximumIntegerValue = (1ULL << 62) - 1ULL;

    /// 1 字节档（前缀 00，可用 6 位）能表示的最大值
    inline constexpr std::uint64_t kQuicMaximumOneByteIntegerValue = 63ULL;

    /// 2 字节档（前缀 01，可用 14 位）能表示的最大值
    inline constexpr std::uint64_t kQuicMaximumTwoByteIntegerValue = 16383ULL;

    /// 4 字节档（前缀 10，可用 30 位）能表示的最大值
    inline constexpr std::uint64_t kQuicMaximumFourByteIntegerValue = 1073741823ULL;

    /**
     * @brief 一个变长整数解码出来的值与它占用的字节数
     *
     * @details 字节数必须随值一起交出：变长整数是报文里逐字段推进的起点，调用方要靠它把读位置挪到下一个字段。
     */
    struct QuicDecodedInteger
    {
        std::uint64_t value{0};   ///< 解出的数值
        std::size_t byteCount{0}; ///< 本数实际占用的字节数（1/2/4/8）
    };

    /**
     * @brief 表示 value 所需的最少字节数
     * @details 定义放在头文件里：档位阈值是编译期常量，编码侧与用例都要能在常量表达式里用。
     * @param value 待表示的数值
     * @return std::size_t 1、2、4 或 8；取值超过 kQuicMaximumIntegerValue 时为 0——本函数不报错，
     *         由编码侧当场拒绝（返回 0 而不是回绕成 1 档，是为了让「超限」在下一层仍可判）
     */
    [[nodiscard]] inline constexpr std::size_t quicVariableLengthIntegerByteCount(const std::uint64_t value) noexcept
    {
        // 四档的可用位数依次是 6/14/30/62，按阈值递降判断即可得出最少需要几字节（RFC 9000 §16 表 4）
        if (value <= kQuicMaximumOneByteIntegerValue)
        {
            return 1;
        }
        if (value <= kQuicMaximumTwoByteIntegerValue)
        {
            return 2;
        }
        if (value <= kQuicMaximumFourByteIntegerValue)
        {
            return 4;
        }
        // 超过 8 字节档的满值就没有合法编码可用：返回 0 让编码侧当场拒绝，不静默截断高位
        return value <= kQuicMaximumIntegerValue ? 8 : 0;
    }

    /**
     * @brief 按最少字节数把一个变长整数追写到缓冲末尾
     * @details 长度域要回填的场合（帧负载长度、报文 Length 域在写出前还不知道最终值）需要占更宽的档，
     *          用带 byteWidth 的重载。
     * @param bytes 目标缓冲，二进制安全
     * @param value 待写入的数值
     * @throws Base::InvalidArgumentException 用法错误：value 超过 kQuicMaximumIntegerValue
     */
    void appendQuicVariableLengthInteger(std::string &bytes, std::uint64_t value);

    /**
     * @brief 按指定宽度把一个变长整数追写到缓冲末尾
     * @details 规范只要求「除帧类型外不必用最短编码」（RFC 9000 §16 末段），因此占宽是合法的；
     *          本重载就是为回填长度域准备的。
     * @param bytes 目标缓冲，二进制安全
     * @param value 待写入的数值
     * @param byteWidth 占用的字节数，只能是 1、2、4、8 四档之一
     * @throws Base::InvalidArgumentException 用法错误：byteWidth 不是 1/2/4/8，或 value 超过该档上限、
     *         超过 kQuicMaximumIntegerValue
     */
    void appendQuicVariableLengthInteger(std::string &bytes, std::uint64_t value, std::size_t byteWidth);

    /**
     * @brief 从字节序列的开头解出一个变长整数
     * @details 按 RFC 9000 附录 A.1 的样例算法实现：首字节高 2 位是「字节数以 2 为底的对数」，
     *          其余位与后续字节按网络序拼成值。**不判非最短编码**（除帧类型外规范允许，见 §16），
     *          帧类型那一档的从严校验归帧解码层做。
     * @param bytes 待解码字节，按「指针 + 长度」取，可含任意二进制
     * @return 成功返回 `QuicDecodedInteger`，其 byteCount 是本数吃掉的字节数（调用方据此前移读位置）
     * @return 失败返回 `QuicDecodeError`：类别恒为 `Truncated`（首字节高 2 位声明的宽度大于剩余字节数）
     */
    [[nodiscard]] std::expected<QuicDecodedInteger, QuicDecodeError>
    decodeQuicVariableLengthInteger(std::span<const std::uint8_t> bytes);
} // namespace AsynGyanis::Net
