/**
 * @file BinaryBytes.h
 * @brief 二进制载荷的规范存储类型、成员类型判定与双向转换
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 二进制列（BLOB / BINARY）在 ORM 侧允许两种等价成员类型：std::vector<std::uint8_t>
 *          （线路字节与各 C API 的通行写法）与 std::vector<std::byte>（表示原始内存的惯用类型）。
 *          本头把「两者是同一种东西」这条规则收在一处：判定、规范化、还原各一份实现，
 *          RowMapper 与 Expression 都从这里取用，避免两处各写一套判断而慢慢漂移。
 *
 * @note 规范存储类型固定为 std::vector<std::uint8_t>：DatabaseValue 与 ParameterValue
 *       的备选都是它，因此 std::byte 成员在映射处做一次逐字节转换。转换是逐元素的
 *       static_cast，不重新解释内存、不改变字节序。
 * @note 刻意**不**接受 std::vector<char> 与 std::string_view：前者与文本载荷无法区分，
 *       后者不持有内存、绑定后生命周期不可控。需要传文本请用 std::string 成员。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace AsynGyanis::Database
{
    /**
     * @brief 二进制载荷的规范存储类型
     * @details 与 DatabaseValue / ParameterValue 中二进制备选的类型一致，可互相直接构造。
     */
    using BinaryBytes = std::vector<std::uint8_t>;

    namespace Detail
    {
        /**
         * @brief 判定成员类型是否为二进制载荷
         * @details 只认 std::vector<std::uint8_t> 与 std::vector<std::byte> 两种拼法。
         *          判定发生在剥掉 std::optional / cv 限定之后，因此 std::optional<BinaryBytes>
         *          也能被识别。
         * @tparam MemberType 结构体成员类型
         */
        template<typename MemberType>
        struct IsBinaryBytes : std::false_type
        {
        };

        /**
         * @brief std::vector<std::uint8_t> 的特化
         */
        template<>
        struct IsBinaryBytes<BinaryBytes> : std::true_type
        {
        };

        /**
         * @brief std::vector<std::byte> 的特化
         */
        template<>
        struct IsBinaryBytes<std::vector<std::byte> > : std::true_type
        {
        };

        /**
         * @brief IsBinaryBytes 的变量模板简写，便于在 if constexpr 中直接使用
         * @tparam MemberType 结构体成员类型
         */
        template<typename MemberType>
        inline constexpr bool kIsBinaryBytes = IsBinaryBytes<MemberType>::value;

        /**
         * @brief 把二进制成员值规范化成 BinaryBytes
         * @details 规范类型自身走拷贝构造；std::vector<std::byte> 逐元素转换——
         *          std::byte 是 enum class，必须显式转成整数才能落进 uint8_t 向量。
         * @tparam MemberType 成员类型（须已判定为二进制载荷）
         * @param value 成员值
         * @return BinaryBytes 规范形态的字节序列
         */
        template<typename MemberType>
        [[nodiscard]] BinaryBytes toBinaryBytes(const MemberType &value)
        {
            if constexpr (std::is_same_v<MemberType, BinaryBytes>)
            {
                return value;
            } else
            {
                // 逐元素转换而不是整块 reinterpret_cast：std::byte 与 uint8_t 是不同类型，
                // 按对象表示重新解释虽然在同一平台上等价，但属于未定义行为，且会随
                // 平台/编译器差异静默出错。这里按值转换，语义明确且可被优化掉
                BinaryBytes bytes;
                bytes.reserve(value.size());
                for (const std::byte rawByte: value)
                {
                    bytes.push_back(static_cast<std::uint8_t>(rawByte));
                }
                return bytes;
            }
        }

        /**
         * @brief 把 BinaryBytes 还原成目标成员类型
         * @details 与 toBinaryBytes 严格互逆，保证「写进去什么、读回来什么」。
         * @tparam MemberType 目标成员类型（须已判定为二进制载荷）
         * @param bytes 规范形态的字节序列
         * @return MemberType 目标成员类型的值
         */
        template<typename MemberType>
        [[nodiscard]] MemberType fromBinaryBytes(const BinaryBytes &bytes)
        {
            if constexpr (std::is_same_v<MemberType, BinaryBytes>)
            {
                return bytes;
            } else
            {
                std::vector<std::byte> rawBytes;
                rawBytes.reserve(bytes.size());
                for (const std::uint8_t byteValue: bytes)
                {
                    rawBytes.push_back(static_cast<std::byte>(byteValue));
                }
                return rawBytes;
            }
        }

        /**
         * @brief 右值入口：目标成员正是规范类型 BinaryBytes 时把字节缓冲直接搬走，免去整块拷贝
         * @details 与 const& 版严格等价，只是当 `MemberType == BinaryBytes`（std::vector<std::uint8_t> 这一常见拼法）
         *          时把来源缓冲 move 进返回值；`std::vector<std::byte>` 成员因元素类型不同仍需逐字节转换，回落 const& 版。
         *          驱动读值与调用方交出所有权的场景（ORM 行映射）据此省掉一次大载荷的堆分配与 memcpy。
         * @tparam MemberType 目标成员类型（须已判定为二进制载荷）
         * @param bytes 即将被搬空的规范字节序列（右值引用）
         * @return MemberType 目标成员类型的值
         */
        template<typename MemberType>
        [[nodiscard]] MemberType fromBinaryBytes(BinaryBytes &&bytes)
        {
            if constexpr (std::is_same_v<MemberType, BinaryBytes>)
            {
                return std::move(bytes);
            }
            else
            {
                return fromBinaryBytes<MemberType>(static_cast<const BinaryBytes &>(bytes));
            }
        }

    } // namespace Detail

} // namespace AsynGyanis::Database
