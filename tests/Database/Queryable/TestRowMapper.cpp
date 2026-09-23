// 覆盖场景（不经过数据库：直接构造 DatabaseValue 调用值转换函数，把每个类型的接受面与拒绝面逐条钉死）：
// - 无符号成员：int64 支路的范围、文本支路的完整上界（2^63、2^64-1）、越界拒绝
// - 文本支路的严格性：负号（对无符号）、小数点、科学计数法、前后空格、空串、尾部余文
// - 有符号成员：int64 支路与文本支路（含 INT64_MIN/INT64_MAX）、越界拒绝
// - 窄整型：uint8/uint32 的目标位宽校验（既有行为，补钉）
// - 拒绝面：浮点不能映射到整型成员，且错误文本说清期望与实际问题
// - 往返：toDatabaseValue 把超出 int64 的无符号值降级为十进制文本，读方向能把这段文本
//   原样解析回来（两个方向的取舍必须配套，否则「写得进、读不回」）
// - 二进制成员：std::vector<std::uint8_t> 与 std::vector<std::byte> 两种拼法必须完全等价
//   （写出同一串字节、读出同一串字节）；0x00..0xFF 全取值与内嵌 '\0' 往返无损；
//   零长载荷是「有值且为空」而非 NULL；optional 包装双向可用；
//   文本或整数落到二进制成员一律报错（列声明与成员声明不一致必须暴露，不能静默收下）
// - bool 成员：只认 0/1 与布尔备选，2/-1 等整数一律拒绝（「非零即真」是静默改值）
// - 浮点成员：整数只接受连续精确区间 ±2^digits 内的取值（越界会取整）；double 收窄进 float 时
//   跨出 float 上下界要报错（会变成无穷大），而本就是 inf/NaN 的取值逐值保真予以接受；
//   文本/布尔/二进制落到浮点成员一律拒绝；digits 达到 int64 宽度的成员（x86 的 long double）
//   整个 int64 区间都精确，窄尾数那一侧仍拒（同一判据、两种结论，MSVC 与 GCC 各取一侧）
// 这些形态在真机上难以稳定构造（后端私自改写的列类型、越界或带余文的数字文本、二进制成员）；整型「十进制文本」
// 支路（引擎存得下、却给不出 int64 的取值只能以文本返回）也靠这里覆盖，真机侧由 MySQL 集成用例验证。

#include "Base/Exception/Exception.h"
#include "Database/Common/BinaryBytes.h"
#include "Database/Common/DatabaseException.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Common/RowMappingException.h"
#include "Database/Queryable/RowMapper.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
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

        /// 2^53：double 能逐位精确表示的整数上界（再大就要取整成邻近的可表示值）
        constexpr std::int64_t kTwoToTheFiftyThird = 9007199254740992LL;

        /// 2^24：float 能逐位精确表示的整数上界
        constexpr std::int64_t kTwoToTheTwentyFourth = 16777216LL;

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

        /**
         * @brief 以浮点形态构造一个数据库值（驱动只在列是 REAL 类存储时给出这一备选）
         * @param realValue 列值
         * @return DatabaseValue 承载该浮点数的统一值
         */
        DatabaseValue realValue(const double realValue)
        {
            return DatabaseValue{realValue};
        }

        /**
         * @brief 以布尔形态构造一个数据库值
         * @param booleanValue 列值
         * @return DatabaseValue 承载该布尔值的统一值
         */
        DatabaseValue booleanValue(const bool booleanValue)
        {
            return DatabaseValue{booleanValue};
        }

    } // namespace

    /**
     * @brief 文本单元格映射进 std::string 成员时走移动：搬空源变体、值逐字节不损
     * @details 钉住 convertDatabaseValue 的右值重载——cellValue 是即将析构的局部量，文本列应把堆缓冲
     *          std::move 进返回值而非再拷一份（省掉的正是「变体→成员」那次整串拷贝与再分配）。
     *          源串被搬空即证明走的是移动；返回值逐字节正确即证明搬的没错、没截断。
     */
    TEST(RowMapperStringMove, MovesTextBufferIntoStringMember)
    {
        DatabaseValue cell{std::string(4096, 'x')};   // 远超短字符串缓冲，是真实堆缓冲
        const std::string converted = Detail::convertDatabaseValue<std::string>(std::move(cell), kColumnName);
        EXPECT_EQ(converted, std::string(4096, 'x'));
        EXPECT_TRUE(std::get<std::string>(cell).empty()) << "源变体的文本缓冲应已被搬空（走移动而非拷贝路径）";
    }

    /**
     * @brief 规范二进制单元格（std::vector<std::uint8_t> 成员）映射走移动：搬空源缓冲、字节不损
     * @details 与文本移动同一条右值重载。规范拼法可无损搬走；断言源被搬空即证明走的是移动而非整块拷贝。
     */
    TEST(RowMapperBinaryMove, MovesCanonicalByteBufferIntoMember)
    {
        const BinaryBytes expected(4096, static_cast<std::uint8_t>(0xAB));   // 真实堆缓冲
        DatabaseValue cell{expected};
        const BinaryBytes converted = Detail::convertDatabaseValue<BinaryBytes>(std::move(cell), kColumnName);
        EXPECT_EQ(converted, expected);
        EXPECT_TRUE(std::get<BinaryBytes>(cell).empty()) << "源二进制缓冲应已被搬空（走移动而非整块拷贝）";
    }

    /**
     * @brief std::byte 拼法的二进制成员仍逐字节转（无法无损搬走），值必须逐字节等价
     * @details 右值重载对 std::vector<std::byte> 目标回落 const& 版本；此例钉「回落不改变取值」，
     *          避免为了移动而错把 std::byte 成员也走 move 分支。
     */
    TEST(RowMapperBinaryMove, ByteSpellingStillRoundTripsThroughMoveEntry)
    {
        const BinaryBytes expected{0x00, 0x7F, 0x80, 0xFF};
        DatabaseValue cell{expected};
        const std::vector<std::byte> converted = Detail::convertDatabaseValue<std::vector<std::byte>>(std::move(cell), kColumnName);
        ASSERT_EQ(converted.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index)
        {
            EXPECT_EQ(static_cast<std::uint8_t>(converted[index]), expected[index]);
        }
    }

    /**
     * @brief 可空文本成员（std::optional<std::string>）非 NULL 时也走移动：搬空源变体、值逐字节不损
     * @details 与裸 std::string 同一条右值重载的可空分支。源串被搬空即证明走的是移动而非「变体→optional
     *          内部」那次整串拷贝；optional 取回值逐字节正确即证明搬的没错。
     */
    TEST(RowMapperStringMove, MovesTextBufferIntoOptionalStringMember)
    {
        DatabaseValue cell{std::string(4096, 'y')};   // 远超短字符串缓冲，是真实堆缓冲
        const std::optional<std::string> converted =
                Detail::convertDatabaseValue<std::optional<std::string>>(std::move(cell), kColumnName);
        ASSERT_TRUE(converted.has_value());
        EXPECT_EQ(*converted, std::string(4096, 'y'));
        EXPECT_TRUE(std::get<std::string>(cell).empty()) << "可空文本列源缓冲应已被搬空（走移动而非拷贝路径）";
    }

    /**
     * @brief 可空文本成员的 NULL 与拒绝面：monostate 落成空 optional，非文本载荷仍按原语义报错
     * @details 移动分支只接管「单元格确实是文本」的情形；NULL（monostate）回落 const& 版得 nullopt，
     *          整数落到 optional<std::string> 仍走列类型不一致的报错——不能因为加了 move 分支就把
     *          拒绝面也放宽。
     */
    TEST(RowMapperStringMove, OptionalStringStillMapsNullToEmptyAndRejectsMismatch)
    {
        DatabaseValue nullCell{std::monostate{}};
        const std::optional<std::string> converted =
                Detail::convertDatabaseValue<std::optional<std::string>>(std::move(nullCell), kColumnName);
        EXPECT_FALSE(converted.has_value()) << "SQL NULL 应映射成空 optional";

        DatabaseValue integerCell{std::int64_t{7}};
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<std::optional<std::string>>(std::move(integerCell), kColumnName)),
                     RowMappingException)
                << "整数落到 optional<std::string> 仍须报列类型不一致，移动分支不得放宽拒绝面";
    }

    /**
     * @brief 可空规范二进制成员也走移动：搬空源缓冲、字节不损
     * @details optional<std::vector<std::uint8_t>> 命中可空二进制分支，源被搬空即证明走移动。
     */
    TEST(RowMapperBinaryMove, MovesCanonicalByteBufferIntoOptionalBinaryMember)
    {
        const BinaryBytes expected(4096, static_cast<std::uint8_t>(0x5A));
        DatabaseValue cell{expected};
        const std::optional<BinaryBytes> converted =
                Detail::convertDatabaseValue<std::optional<BinaryBytes>>(std::move(cell), kColumnName);
        ASSERT_TRUE(converted.has_value());
        EXPECT_EQ(*converted, expected);
        EXPECT_TRUE(std::get<BinaryBytes>(cell).empty()) << "可空二进制列源缓冲应已被搬空";
    }

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
     * @details 映射失败抛的是 RowMappingException，上溯到 DatabaseException、Base::Exception 与 std::runtime_error
     *          四层都成立——本条用例把这条链一次钉死，调用方才能用一条 catch (const Base::Exception &) 兜住运行期故障。
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

    /**
     * @brief 验证 bool 成员接受 0/1 整数与布尔备选，NULL 经 optional 映射成空
     *
     * @details SQLite 没有布尔存储类，布尔列一律以 INTEGER 的 0/1 落库，因此整数 0/1 必须能读回 bool。
     */
    TEST(RowMapperBool, AcceptsZeroAndOneAndTheBoolAlternative)
    {
        EXPECT_FALSE(Detail::convertDatabaseValue<bool>(integerValue(0), kColumnName));
        EXPECT_TRUE(Detail::convertDatabaseValue<bool>(integerValue(1), kColumnName));
        EXPECT_TRUE(Detail::convertDatabaseValue<bool>(booleanValue(true), kColumnName));
        EXPECT_FALSE(Detail::convertDatabaseValue<bool>(booleanValue(false), kColumnName));

        // 可空布尔列：NULL 是「没有值」而不是 false，必须落在空 optional 上
        EXPECT_FALSE(Detail::convertDatabaseValue<std::optional<bool>>(DatabaseValue{}, kColumnName).has_value());
        EXPECT_TRUE(Detail::convertDatabaseValue<std::optional<bool>>(integerValue(1), kColumnName).value_or(false));
    }

    /**
     * @brief 验证 bool 成员拒绝 0/1 之外的整数，而不是按「非零即真」静默收下
     *
     * @details 取值 2/-1/7 塞进 bool 会静默变成 true：调用方拿到一个「看起来正常」的对象，
     *          而真相是这一列根本不是布尔列（成员声明与列声明已经不符）。窄化整族一律要显式失败。
     */
    TEST(RowMapperBool, RejectsIntegersOutsideZeroAndOne)
    {
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<bool>(integerValue(2), kColumnName)), RowMappingException);
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<bool>(integerValue(7), kColumnName)), RowMappingException);
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<bool>(integerValue(-1), kColumnName)), RowMappingException);
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<bool>(integerValue(std::numeric_limits<std::int64_t>::min()), kColumnName)),
                     RowMappingException);

        // 文本形态同样拒绝：本方法只认布尔备选与 0/1 整数
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<bool>(textValue("1"), kColumnName)), RowMappingException);

        // 错误文本要能定位到列并给出替代做法，否则调用方只能自己去猜
        try
        {
            static_cast<void>(Detail::convertDatabaseValue<bool>(integerValue(7), kColumnName));
            FAIL() << "7 映射为 bool 应当失败";
        }
        catch (const RowMappingException &failure)
        {
            EXPECT_NE(std::string_view{failure.what()}.find("big"), std::string_view::npos) << failure.what();
            EXPECT_NE(std::string_view{failure.what()}.find("整型"), std::string_view::npos) << failure.what();
        }
    }

    /**
     * @brief 验证浮点成员接受能逐位精确表示的整数（含连续精确区间的两侧端点）
     *
     * @details 整数值的 REAL 列在 SQLite 里会回传 INTEGER 备选，因此这条支路必须存在；
     *          端点取值 2^53（double）与 2^24（float）本身可精确表示，属于接受面。
     */
    TEST(RowMapperFloating, AcceptsIntegersUpToTheExactRepresentableBound)
    {
        EXPECT_DOUBLE_EQ(static_cast<double>(kTwoToTheFiftyThird), Detail::convertDatabaseValue<double>(integerValue(kTwoToTheFiftyThird), kColumnName));
        EXPECT_DOUBLE_EQ(-static_cast<double>(kTwoToTheFiftyThird),
                         Detail::convertDatabaseValue<double>(integerValue(-kTwoToTheFiftyThird), kColumnName));
        EXPECT_FLOAT_EQ(static_cast<float>(kTwoToTheTwentyFourth),
                        Detail::convertDatabaseValue<float>(integerValue(kTwoToTheTwentyFourth), kColumnName));
        EXPECT_DOUBLE_EQ(1.0, Detail::convertDatabaseValue<double>(integerValue(1), kColumnName));
    }

    /**
     * @brief 验证浮点成员拒绝超出连续精确区间的整数，而不是静默取整
     *
     * @details 2^53+1 转成 double 会得到 2^53（浮点装不下那个奇数），2^24+1 转成 float 同理——
     *          改写后它「是个数」但不再是那个数。整型一侧早已按同一口径拒绝越界，浮点一侧不能放宽。
     */
    TEST(RowMapperFloating, RejectsIntegersBeyondTheExactRepresentableBound)
    {
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<double>(integerValue(kTwoToTheFiftyThird + 1), kColumnName)),
                     RowMappingException);
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<double>(integerValue(std::numeric_limits<std::int64_t>::max()), kColumnName)),
                     RowMappingException);
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<float>(integerValue(kTwoToTheTwentyFourth + 1), kColumnName)),
                     RowMappingException);
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<float>(integerValue(-16777217), kColumnName)), RowMappingException);
    }

    /**
     * @brief 钉住精确区间按成员类型自己的 digits 判：宽尾数类型能吃下整个 int64 区间
     * @details 尾数位等于或超过 int64 宽度的类型（x86-64 的 long double 有 64 位有效位）不存在
     *          「装不下某个 int64」的情况，边界因此就是 int64 本身；而窄尾数类型（MSVC 上 long double
     *          等同 double，53 位）仍要拒绝越界取值。两边同一判据、不同结论，用编译期分支各钉一侧。
     * @warning 这条用例的证伪只能在容器里做：把区间换算改回「1 左移 digits」，GCC 侧是编译期非法移位
     *          （整棵测试树编不过），MSVC 侧因 long double 就是 double 而看不出差别。
     */
    TEST(RowMapperFloating, JudgesExactnessByTheMemberTypeOwnDigitCount)
    {
        // 两侧共同的接受面：2^53 在任何浮点成员上都精确
        EXPECT_EQ(static_cast<long double>(kTwoToTheFiftyThird),
                  Detail::convertDatabaseValue<long double>(integerValue(kTwoToTheFiftyThird), kColumnName));

        if constexpr (std::numeric_limits<long double>::digits >= std::numeric_limits<std::int64_t>::digits)
        {
            const std::int64_t maximumSigned = std::numeric_limits<std::int64_t>::max();
            EXPECT_EQ(static_cast<long double>(maximumSigned),
                      Detail::convertDatabaseValue<long double>(integerValue(maximumSigned), kColumnName));
            // double 装不下的那个奇数，宽尾数类型仍然逐位精确
            constexpr std::int64_t beyondDoubleExactness = -9007199254740993LL;
            EXPECT_EQ(static_cast<long double>(beyondDoubleExactness),
                      Detail::convertDatabaseValue<long double>(integerValue(beyondDoubleExactness), kColumnName));
        }
        else
        {
            EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<long double>(
                             integerValue(std::numeric_limits<std::int64_t>::max()), kColumnName)),
                         RowMappingException);
        }
    }

    /**
     * @brief 验证 double 收窄进 float 时溢出被判失败，而原本就是无穷大/NaN 的取值逐值保真
     *
     * @details 1e300 转成 float 会得到 +inf——不是「精度差一点」而是换了一个数，必须拒绝；
     *          inf/NaN 转成 float 仍是 inf/NaN，属于无损转换，予以接受（成员既然声明为 float
     *          就要能读回引擎给出的无穷大）。尾数精度损失（3.14159… → 3.14159f）是声明 float 的既有取舍。
     */
    TEST(RowMapperFloating, RejectsFloatOverflowButKeepsInfiniteAndNotANumber)
    {
        EXPECT_FLOAT_EQ(3.4e38F, Detail::convertDatabaseValue<float>(realValue(3.4e38), kColumnName));
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<float>(realValue(1e300), kColumnName)), RowMappingException);
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<float>(realValue(-1e300), kColumnName)), RowMappingException);
        // double 成员没有这条上界问题：同一个取值原样收下
        EXPECT_DOUBLE_EQ(1e300, Detail::convertDatabaseValue<double>(realValue(1e300), kColumnName));

        const double infinity = std::numeric_limits<double>::infinity();
        EXPECT_TRUE(std::isinf(Detail::convertDatabaseValue<float>(realValue(infinity), kColumnName)));
        EXPECT_TRUE(std::isinf(Detail::convertDatabaseValue<double>(realValue(-infinity), kColumnName)));
        EXPECT_TRUE(std::isnan(Detail::convertDatabaseValue<float>(realValue(std::numeric_limits<double>::quiet_NaN()), kColumnName)));
    }

    /**
     * @brief 验证文本与二进制载荷不会为了「跑通」而被当成浮点收下
     *
     * @details 与整型一侧不同：浮点没有「引擎给不出该取值只能以文本返回」的必需支路，
     *          文本落到浮点成员说明列声明与成员声明已经不符，一律按列类型错误报告。
     */
    TEST(RowMapperFloating, RejectsTextAndBinaryForFloatingMember)
    {
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<double>(textValue("1.5"), kColumnName)), RowMappingException);
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<double>(booleanValue(true), kColumnName)), RowMappingException);
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<double>(DatabaseValue{BinaryBytes{0x01}}, kColumnName)), RowMappingException);
        EXPECT_THROW(static_cast<void>(Detail::convertDatabaseValue<double>(DatabaseValue{}, kColumnName)), RowMappingException);
    }

} // namespace AsynGyanis::Database::Queryable
