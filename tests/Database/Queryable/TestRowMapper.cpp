/**
 * @file TestRowMapper.cpp
 * @brief 行映射的值转换单元测试 —— 直接驱动 Detail::convertDatabaseValue / toDatabaseValue
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 与其它 Queryable 用例不同，本文件**不经过数据库**：直接构造 DatabaseValue 调用
 *          值转换函数，因此能把每个类型的接受面与拒绝面逐条钉死，尤其是那些真机上
 *          难以稳定构造出来的形态（后端私自改写的列类型、越界文本、带余文的数字文本）。
 *
 *          本轮新增的整型「十进制文本」支路正是靠这里覆盖：引擎存得下、却给不出 int64 的
 *          整数只能以文本返回（如 MySQL 的 BIGINT UNSIGNED 上界），
 *          没有这一支就会出现「写得进去、读不回来」。真机侧另有集成用例（MySQL）验证同一件事。
 *
 * 覆盖场景：
 * - 无符号成员：int64 支路的范围、文本支路的完整上界（2^63、2^64-1）、越界拒绝
 * - 文本支路的严格性：负号（对无符号）、小数点、科学计数法、前后空格、空串、尾部余文
 * - 有符号成员：int64 支路与文本支路（含 INT64_MIN/INT64_MAX）、越界拒绝
 * - 窄整型：uint8/uint32 的目标位宽校验（既有行为，补钉）
 * - 拒绝面：浮点不能映射到整型成员，且错误文本说清期望与实际问题
 * - 往返：toDatabaseValue 把超出 int64 的无符号值降级为十进制文本，读方向能把这段文本
 *   原样解析回来（两个方向的取舍必须配套，否则「写得进、读不回」）
 * - 二进制成员：std::vector<std::uint8_t> 与 std::vector<std::byte> 两种拼法必须完全等价
 *   （写出同一串字节、读出同一串字节）；0x00..0xFF 全取值与内嵌 '\0' 往返无损；
 *   零长载荷是「有值且为空」而非 NULL；optional 包装双向可用；
 *   文本或整数落到二进制成员一律报错（列声明与成员声明不一致必须暴露，不能静默收下）
 */
#include "Base/Exception/Exception.h"
#include "Database/Common/BinaryBytes.h"
#include "Database/Common/DatabaseException.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Common/RowMappingException.h"
#include "Database/Queryable/RowMapper.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace AsynGyanis::Database::Queryable
{
    namespace
    {
        /// 测试用列名：错误信息里会带上它，用于确认报错能定位到具体列
        constexpr std::string_view kColumnName = "big";

        /// 2^63（INT64_MAX + 1）：int64 表示不了、uint64 表示得了的第一个取值
        constexpr std::uint64_t kTwoToTheSixtyThird = 9223372036854775808ULL;

        /// 2^64 - 1：uint64 的上界，也是 MySQL BIGINT UNSIGNED 的上界
        constexpr std::uint64_t kMaximumUInt64 = std::numeric_limits<std::uint64_t>::max();

        /**
         * @brief 以文本形态构造一个数据库值
         * @param textValue 列值文本
         * @return DatabaseValue 承载该文本的统一值
         */
        DatabaseValue textValue(const std::string &textValue)
        {
            return DatabaseValue{textValue};
        }

        /**
         * @brief 以 64 位有符号整数形态构造一个数据库值
         * @param integerValue 列值
         * @return DatabaseValue 承载该整数的统一值
         */
        DatabaseValue integerValue(const std::int64_t integerValue)
        {
            return DatabaseValue{integerValue};
        }

    } // namespace

    // ------------------------------------------------------------------------
    // 无符号成员：int64 支路
    // ------------------------------------------------------------------------

    /**
     * @brief 验证 int64 能覆盖的无符号取值原样映射，且负值被拒
     */
    TEST(RowMapperUnsigned, AcceptsInt64WithinUnsignedRange)
    {
        EXPECT_EQ(0U, Detail::convertDatabaseValue<std::uint64_t>(integerValue(0), kColumnName));
        EXPECT_EQ(42U, Detail::convertDatabaseValue<std::uint64_t>(integerValue(42), kColumnName));

        // INT64_MAX 是 int64 支路能给出的最大正值，必须无损地成为 uint64
        const auto maximumSigned = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        EXPECT_EQ(maximumSigned, Detail::convertDatabaseValue<std::uint64_t>(integerValue(std::numeric_limits<std::int64_t>::max()), kColumnName));

        // 负值在无符号成员上无从表达：静默回绕成巨大正数是能藏最久的错误
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<std::uint64_t>(integerValue(-1), kColumnName)), std::runtime_error);
    }

    // ------------------------------------------------------------------------
    // 无符号成员：十进制文本支路
    // ------------------------------------------------------------------------

    /**
     * @brief 验证文本支路能吃下 int64 之外、uint64 之内的取值
     */
    TEST(RowMapperUnsigned, AcceptsTextBeyondInt64Range)
    {
        // 这两段文本正是 MySQL 的 BIGINT UNSIGNED 在超出 int64 后会返回的形态
        EXPECT_EQ(kTwoToTheSixtyThird,
                  Detail::convertDatabaseValue<std::uint64_t>(textValue("9223372036854775808"), kColumnName));
        EXPECT_EQ(kMaximumUInt64,
                  Detail::convertDatabaseValue<std::uint64_t>(textValue("18446744073709551615"), kColumnName));
    }

    /**
     * @brief 验证文本支路在边界附近严格按位宽拒绝
     */
    TEST(RowMapperUnsigned, RejectsTextBeyondTargetWidth)
    {
        // 2^64：比 uint64 上界大 1，必须拒绝而不是回绕成 0
        EXPECT_THROW(
            static_cast<void>(Detail::convertDatabaseValue<std::uint64_t>(textValue("18446744073709551616"), kColumnName)),
            std::runtime_error);

        // 32 位无符号成员同样按自己的上界判定（文本支路不能只看 uint64）
        EXPECT_EQ(4294967295U, Detail::convertDatabaseValue<std::uint32_t>(textValue("4294967295"), kColumnName));
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<std::uint32_t>(textValue("4294967296"), kColumnName)), std::runtime_error);
    }

    /**
     * @brief 验证文本支路只接受纯十进制整数
     */
    TEST(RowMapperUnsigned, RejectsNonIntegerText)
    {
        // 无符号成员收到负号：它不是「越界」而是根本不适用，因此与前一组用例分开钉
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<std::uint64_t>(textValue("-1"), kColumnName)), std::runtime_error);

        // 小数点与科学计数法：取整会静默改变数值（1.9 → 1），因此必须拒绝
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<std::uint64_t>(textValue("1.9"), kColumnName)), std::runtime_error);
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<std::uint64_t>(textValue("1e3"), kColumnName)), std::runtime_error);

        // 前后空格与尾部余文：from_chars 不跳空白，因此这些都不是合法整数
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<std::uint64_t>(textValue(" 12"), kColumnName)), std::runtime_error);
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<std::uint64_t>(textValue("12 "), kColumnName)), std::runtime_error);
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<std::uint64_t>(textValue("12abc"), kColumnName)), std::runtime_error);
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<std::uint64_t>(textValue(""), kColumnName)), std::runtime_error);
    }

    // ------------------------------------------------------------------------
    // 有符号成员
    // ------------------------------------------------------------------------

    /**
     * @brief 验证有符号成员的文本支路（含两端边界）
     */
    TEST(RowMapperSigned, AcceptsTextWithinInt64Range)
    {
        EXPECT_EQ(-1, Detail::convertDatabaseValue<std::int64_t>(textValue("-1"), kColumnName));
        EXPECT_EQ(0, Detail::convertDatabaseValue<std::int64_t>(textValue("0"), kColumnName));
        EXPECT_EQ(std::numeric_limits<std::int64_t>::max(),
                  Detail::convertDatabaseValue<std::int64_t>(textValue("9223372036854775807"), kColumnName));
        EXPECT_EQ(std::numeric_limits<std::int64_t>::min(),
                  Detail::convertDatabaseValue<std::int64_t>(textValue("-9223372036854775808"), kColumnName));
    }

    /**
     * @brief 验证有符号成员在文本支路上同样拒绝越界与非整数文本
     */
    TEST(RowMapperSigned, RejectsTextOutOfRangeOrMalformed)
    {
        // 2^63 放不进 int64：即便它是 uint64 的有效取值，对 int64 成员也必须失败
        EXPECT_THROW(
            static_cast<void>(Detail::convertDatabaseValue<std::int64_t>(textValue("9223372036854775808"), kColumnName)),
            std::runtime_error);
        EXPECT_THROW(
            static_cast<void>(Detail::convertDatabaseValue<std::int64_t>(textValue("-9223372036854775809"), kColumnName)),
            std::runtime_error);

        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<std::int64_t>(textValue("1.5"), kColumnName)), std::runtime_error);
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<std::int64_t>(textValue("abc"), kColumnName)), std::runtime_error);
    }

    /**
     * @brief 验证窄整型在文本支路上仍按目标位宽校验（uint8 的上界是 255）
     */
    TEST(RowMapperNarrow, AppliesTargetWidthToTextPath)
    {
        EXPECT_EQ(255U, Detail::convertDatabaseValue<std::uint8_t>(textValue("255"), kColumnName));
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<std::uint8_t>(textValue("256"), kColumnName)), std::runtime_error);

        // int64 支路上的窄化校验是既有行为，这里补钉一条，保证两条支路判定一致
        EXPECT_EQ(static_cast<std::int8_t>(-128), Detail::convertDatabaseValue<std::int8_t>(integerValue(-128), kColumnName));
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<std::int8_t>(integerValue(300), kColumnName)), std::runtime_error);
    }

    // ------------------------------------------------------------------------
    // 拒绝面与错误文本
    // ------------------------------------------------------------------------

    /**
     * @brief 验证浮点列不会映射到整型成员，且错误文本说清期望与实际问题
     */
    TEST(RowMapperRejection, RejectsFloatingPointForIntegralMember)
    {
        try
        {
            static_cast<void>(Detail::convertDatabaseValue<std::uint64_t>(DatabaseValue{1.5}, kColumnName));
            FAIL() << "浮点值映射到无符号整型成员本应抛异常";
        } catch (const std::runtime_error &error)
        {
            const std::string message = error.what();
            // 期望类型里必须提到文本形态：否则使用者会以为只需让引擎返回整数，
            // 而实际上 MySQL/PG 在超大整数上只能给文本
            EXPECT_NE(message.find("无符号整型"), std::string::npos) << message;
            EXPECT_NE(message.find("十进制文本"), std::string::npos) << message;
            EXPECT_NE(message.find(std::string(kColumnName)), std::string::npos) << message;
            EXPECT_NE(message.find("Double"), std::string::npos) << message;
        }
    }

    /**
     * @brief 验证 optional 成员的 NULL 与非 NULL 两条路径
     */
    TEST(RowMapperOptional, MapsNullToEmptyAndTextToValue)
    {
        EXPECT_FALSE(Detail::convertDatabaseValue<std::optional<std::uint64_t>>(DatabaseValue{std::monostate{}}, kColumnName)
                         .has_value());
        EXPECT_EQ(kMaximumUInt64,
                  Detail::convertDatabaseValue<std::optional<std::uint64_t>>(textValue("18446744073709551615"), kColumnName));
        EXPECT_EQ(7U, Detail::convertDatabaseValue<std::optional<std::uint32_t>>(integerValue(7), kColumnName));
    }

    // ------------------------------------------------------------------------
    // 写读对称
    // ------------------------------------------------------------------------

    /**
     * @brief 验证「写方向的降级」与「读方向的解析」严格互补
     *
     * @details 写方向把超出 int64 的无符号值降级为十进制文本（DatabaseValue 没有无符号备选），
     *          读方向必须能把同一段文本解析回原值。这条用例把两个方向接在一起跑一遍，
     *          因为任何一边单独改动都会让整型往返失效——而失效的表现是「写得进、读不回」，
     *          只在真机跨进程时才暴露。
     */
    TEST(RowMapperSymmetry, WriteDegradationIsReadableByTextPath)
    {
        // 超出 int64：写方向降级成文本，读方向按文本解析回来
        const DatabaseValue maximumAsParameter = Detail::toDatabaseValue<std::uint64_t>(kMaximumUInt64);
        ASSERT_TRUE(std::holds_alternative<std::string>(maximumAsParameter));
        EXPECT_EQ(kMaximumUInt64, Detail::convertDatabaseValue<std::uint64_t>(maximumAsParameter, kColumnName));

        // int64 范围内：写方向按整数绑定，读方向走 int64 支路
        const DatabaseValue smallAsParameter = Detail::toDatabaseValue<std::uint64_t>(7U);
        ASSERT_TRUE(std::holds_alternative<std::int64_t>(smallAsParameter));
        EXPECT_EQ(7U, Detail::convertDatabaseValue<std::uint64_t>(smallAsParameter, kColumnName));

        // optional 空值：写方向绑定 NULL，读方向得到空 optional
        const DatabaseValue nullParameter = Detail::toDatabaseValue<std::optional<std::uint64_t>>(std::nullopt);
        ASSERT_TRUE(std::holds_alternative<std::monostate>(nullParameter));
        EXPECT_FALSE(Detail::convertDatabaseValue<std::optional<std::uint64_t>>(nullParameter, kColumnName).has_value());
    }

    // ------------------------------------------------------------------------
    // 二进制成员：两种拼法等价
    // ------------------------------------------------------------------------

    /**
     * @brief 验证两种二进制成员拼法写出的载荷逐字节相同
     *
     * @details std::vector<std::uint8_t> 与 std::vector<std::byte> 只是「按不按枚举类访问」
     *          的区别，落库后必须是同一串字节；否则同一个值用不同拼法声明就会写出不同数据。
     */
    TEST(RowMapperBinary, BothMemberSpellingsProduceTheSameBytes)
    {
        const BinaryBytes            canonical{0x00, 0x5C, 0xFF};
        const std::vector<std::byte> rawBytes{std::byte{0x00}, std::byte{0x5C}, std::byte{0xFF}};

        const DatabaseValue fromCanonical = Detail::toDatabaseValue<BinaryBytes>(canonical);
        const DatabaseValue fromRawBytes  = Detail::toDatabaseValue<std::vector<std::byte>>(rawBytes);

        ASSERT_TRUE(std::holds_alternative<BinaryBytes>(fromCanonical)) << databaseValueTypeName(fromCanonical);
        ASSERT_TRUE(std::holds_alternative<BinaryBytes>(fromRawBytes)) << databaseValueTypeName(fromRawBytes);
        EXPECT_EQ(std::get<BinaryBytes>(fromCanonical), std::get<BinaryBytes>(fromRawBytes));
        EXPECT_EQ(canonical, std::get<BinaryBytes>(fromRawBytes));
    }

    /**
     * @brief 验证全部字节取值与内嵌 '\0' 在两种拼法下都往返无损
     */
    TEST(RowMapperBinary, RoundTripsEveryByteValueForBothSpellings)
    {
        BinaryBytes allByteValues;
        allByteValues.reserve(256);
        for (int byteValue = 0; byteValue < 256; ++byteValue)
        {
            allByteValues.push_back(static_cast<std::uint8_t>(byteValue));
        }

        const DatabaseValue payload = Detail::toDatabaseValue<BinaryBytes>(allByteValues);
        EXPECT_EQ(allByteValues, Detail::convertDatabaseValue<BinaryBytes>(payload, kColumnName));

        // std::byte 拼法读同一份载荷：逐元素转换必须无损，否则两种拼法就不等价
        const std::vector<std::byte> rawSpelling =
            Detail::convertDatabaseValue<std::vector<std::byte>>(payload, kColumnName);
        ASSERT_EQ(rawSpelling.size(), allByteValues.size());
        for (std::size_t index = 0; index < allByteValues.size(); ++index)
        {
            EXPECT_EQ(static_cast<std::uint8_t>(rawSpelling[index]), allByteValues[index])
                << "第 " << index << " 个字节转换后不一致";
        }
    }

    /**
     * @brief 验证零长载荷是「有值且为空」，不会被当成 NULL
     *
     * @details 空 BLOB 与 SQL NULL 在数据库里是两件事，在映射层也必须保持这个区分：
     *          只有 monostate 才该变成空 optional。
     */
    TEST(RowMapperBinary, EmptyPayloadIsNotConfusedWithNull)
    {
        const DatabaseValue emptyPayload = Detail::toDatabaseValue<BinaryBytes>(BinaryBytes{});
        ASSERT_TRUE(std::holds_alternative<BinaryBytes>(emptyPayload)) << databaseValueTypeName(emptyPayload);
        EXPECT_TRUE(std::get<BinaryBytes>(emptyPayload).empty());
        EXPECT_TRUE(Detail::convertDatabaseValue<BinaryBytes>(emptyPayload, kColumnName).empty());

        const DatabaseValue nullValue{std::monostate{}};
        EXPECT_FALSE(Detail::convertDatabaseValue<std::optional<BinaryBytes>>(nullValue, kColumnName).has_value());
        EXPECT_TRUE(Detail::convertDatabaseValue<std::optional<BinaryBytes>>(emptyPayload, kColumnName).has_value());
    }

    /**
     * @brief 验证 optional 包装的二进制成员在两个方向上都可用
     */
    TEST(RowMapperBinary, OptionalWrappingIsSupportedInBothDirections)
    {
        const BinaryBytes payload{0x01, 0x02};

        const DatabaseValue written =
            Detail::toDatabaseValue<std::optional<BinaryBytes>>(std::optional<BinaryBytes>(payload));
        ASSERT_TRUE(std::holds_alternative<BinaryBytes>(written)) << databaseValueTypeName(written);

        const std::optional<BinaryBytes> readBack =
            Detail::convertDatabaseValue<std::optional<BinaryBytes>>(written, kColumnName);
        ASSERT_TRUE(readBack.has_value());
        EXPECT_EQ(payload, readBack.value());

        // 空 optional 绑定 NULL 而不是零长载荷
        const DatabaseValue nullWritten = Detail::toDatabaseValue<std::optional<BinaryBytes>>(std::nullopt);
        ASSERT_TRUE(std::holds_alternative<std::monostate>(nullWritten)) << databaseValueTypeName(nullWritten);
    }

    /**
     * @brief 验证文本与整数都不会被当成二进制收下
     */
    TEST(RowMapperBinary, RejectsTextAndIntegerForBinaryMember)
    {
        // 列产出文本而成员声明为二进制，说明列的声明与成员的声明已经不一致。
        // 收下它能让代码「跑通」，却把 schema 漂移一路藏到按二进制语义解读文本数据的那天
        try
        {
            static_cast<void>(Detail::convertDatabaseValue<BinaryBytes>(textValue("not binary"), kColumnName));
            FAIL() << "文本不应映射到二进制成员";
        }
        catch (const std::runtime_error &error)
        {
            const std::string message = error.what();
            EXPECT_NE(message.find("big"), std::string::npos) << message;
            EXPECT_NE(message.find("二进制"), std::string::npos) << message;
        }

        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<BinaryBytes>(integerValue(7), kColumnName)),
                     std::runtime_error);
    }

    /**
     * @brief 验证二进制成员不会退化成文本备选
     *
     * @details 这一点与无符号整数不同：整型超出 int64 时可以降级为十进制文本（引擎给不出
     *          足够宽的整数），但二进制没有这条退路——一旦退成文本，驱动就只能按连接字符集
     *          绑定载荷，字节可能被替换。类型必须一路保持为二进制。
     */
    TEST(RowMapperBinary, DoesNotDegradeToTextAlternative)
    {
        const DatabaseValue payload = Detail::toDatabaseValue<BinaryBytes>(BinaryBytes{0xFF});
        EXPECT_FALSE(std::holds_alternative<std::string>(payload)) << databaseValueTypeName(payload);
        EXPECT_TRUE(std::holds_alternative<BinaryBytes>(payload)) << databaseValueTypeName(payload);
    }

    /**
     * @brief 验证真实的映射失败能被框架异常基类捕获
     *
     * @details 改造前这里抛的是裸 std::runtime_error：调用方即使只想「兜住框架的运行期故障」
     *          也拿不到任何框架类型可捕，只能退化成 catch (std::exception)。现在它是
     *          RowMappingException，上溯到 DatabaseException、Base::Exception 与
     *          std::runtime_error 四层都成立——本条用例把这条链一次钉死。
     */
    TEST(RowMapperException, MappingFailureIsCatchableThroughProjectAndModuleBases)
    {
        const auto throwMappingFailure = []
        {
            static_cast<void>(Detail::convertDatabaseValue<std::int64_t>(textValue("abc"), kColumnName));
        };

        EXPECT_THROW(throwMappingFailure(), RowMappingException);
        EXPECT_THROW(throwMappingFailure(), DatabaseException);
        EXPECT_THROW(throwMappingFailure(), Base::Exception);
        EXPECT_THROW(throwMappingFailure(), std::runtime_error);
    }

} // namespace AsynGyanis::Database::Queryable
