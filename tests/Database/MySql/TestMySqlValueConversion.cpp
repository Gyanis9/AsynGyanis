// MySqlValueConversion 直测：它是驱动里「按列声明类型把文本解析成 DatabaseValue」的唯一实现，
// 纯函数、不接触任何句柄，因此无需服务端也能把全部判据钉死。它必须看到真实 mysql.h 才能拿到
// enum_field_types 常量，故整份按 DATABASE_HAS_MYSQL 门控：没有 libmysqlclient 的配置下本文件
// 只留一条 SKIP 用例，不去猜那些枚举的数值。
//
// 覆盖场景：
// - parseIntegerText：int64 上下界；空白/小数点/科学计数/十六进制/余文/越界一律拒；
//   **带前导 '+' 也拒**（std::from_chars 的符号位只认负号，而 MySQL 印数不带正号，故无需放宽）
// - parseDoubleText：十进制与科学计数、±0、上溢拒；非有限值拼写 inf / -inf / nan / -nan 及
//   infinity 由 from_chars 的浮点文法认出（这点反直觉，故单独钉用例）
// - isBinaryColumn：BLOB 与 TEXT 共用类型码，字符集 63 是唯一判据；非字节类型即使字符集是 binary 也不按二进制
// - convertColumnText：整数列、浮点列、DECIMAL 交原文、零长 BLOB ≠ NULL、内嵌 '\0' 按长度保留；
//   BIT 列按大端整数还原（BIT(64) 超 int64 的那半段退回十进制文本，意外宽度一律不猜）

#include "Database/Common/BinaryBytes.h"
#include "Database/Common/DatabaseValue.h"

#include <gtest/gtest.h>

#ifdef DATABASE_HAS_MYSQL

// 被量头文件带着 mysql.h，因此它的 include 也必须落在门控里面：写在门外时，
// 未编译驱动的配置（-DDATABASE_WITH_MYSQL=OFF）连本文件都编不过
#include "Database/MySql/MySqlValueConversion.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace AsynGyanis::Database::Detail
{
    namespace
    {
        /// utf8mb4 之类的文本字符集编号：用来证明「类型码是 BLOB 但字符集不是 binary」时不算二进制列
        constexpr unsigned int kTextCharacterSetNumber = 33U;

        /**
         * @brief 按「指针 + 长度」调用只吃这两个入参的被量函数
         * @details 形参模板deduce 的是可调用对象本身而不是它的返回类型：写成
         *          Result (&)(const char *, std::size_t) 会把 const 归到返回类型上，推导失败。
         * @tparam Function 被量函数类型
         * @param text 待送入的列值文本，可内嵌 '\0'
         * @param invoke 真正的调用
         * @return decltype(invoke(...)) 被量函数的结果
         */
        template<typename Function>
        [[nodiscard]] auto withRawText(const std::string_view text, const Function &invoke)
        {
            return invoke(text.data(), text.size());
        }
    } // namespace

    /**
     * @brief 验证整数文本解析覆盖 int64 的两端
     * @details BIGINT 列的取值域正好是 int64，端点判错会表现为「最大取值读不回来」。
     */
    TEST(MySqlValueConversionInteger, AcceptsTheWholeSignedSixtyFourBitRange)
    {
        for (const std::string_view text: {"0", "-1", "42", "9223372036854775807", "-9223372036854775808"})
        {
            const std::optional<std::int64_t> parsed = withRawText(text, parseIntegerText);
            ASSERT_TRUE(parsed.has_value()) << text;
            EXPECT_EQ(*parsed, std::stoll(std::string(text))) << text;
        }
    }

    /**
     * @brief 验证整数文本解析的拒绝面：不吞空白、不取整、不截断余文、不静默回绕
     * @details 这些形态一旦放宽，就会把「列里存的其实是别的类型」悄悄当成 0 或半个数字交出去。
     *          "+42" 也在拒绝面内：std::from_chars 的符号位只认负号，而 MySQL 印整数从不带正号，
     *          因此这条不是缺口——把它写成用例是为了让「谁将来想放宽」时先撞上这条已记录的判定。
     */
    TEST(MySqlValueConversionInteger, RejectsAnythingThatIsNotAPureDecimalInteger)
    {
        for (const std::string_view text:
             {"", " ", " 12", "12 ", "1.5", "1e3", "0x10", "12abc", "+", "-", "+42", "9223372036854775808", "-9223372036854775809", "99999999999999999999999999"})
        {
            EXPECT_FALSE(withRawText(text, parseIntegerText).has_value()) << "\"" << text << "\" 不该被当成整数";
        }
    }

    /**
     * @brief 验证浮点文本解析覆盖常规写法，并把上溢判为失败而不是造出无穷大
     * @details 「1e400」在 C++ 数值文法里是 out_of_range；把它当 inf 交出等于替调用方决定
     *          「这个装不下的数就是无穷大」，因此退回原文由上层按列类型报错。
     */
    TEST(MySqlValueConversionDouble, AcceptsFiniteFormsAndRejectsOverflow)
    {
        for (const std::string_view text: {"0", "-0", "3.5", "1e3", "-2.5E-3", "1.7976931348623157e308"})
        {
            const std::optional<double> parsed = withRawText(text, parseDoubleText);
            ASSERT_TRUE(parsed.has_value()) << text;
            EXPECT_DOUBLE_EQ(*parsed, std::stod(std::string(text))) << text;
        }

        for (const std::string_view text: {"", "1e400", "-1e400", "abc", "1.5 ", " 1.5", "1.5x", "--1", "1.2.3", "+7"})
        {
            EXPECT_FALSE(withRawText(text, parseDoubleText).has_value()) << "\"" << text << "\" 不该被当成有限浮点";
        }
    }

    /**
     * @brief 验证服务端的非有限值文本被还原成对应的 double
     * @details 反直觉之处：std::from_chars 的浮点重载按标准给的浮点文法解析，其中就含
     *          inf / infinity / nan（大小写不限），所以 MySQL 印出的 inf / -inf / nan / -nan
     *          在驱动里本来就是可恢复的，不需要额外的拼写表。这条用例钉住的是「不必放宽」这个事实：
     *          若将来有人给 parseDoubleText 加一张拼写白名单，这里会立刻暴露它把标准文法收窄了。
     */
    TEST(MySqlValueConversionDouble, RecoversTheServersNonFiniteSpellings)
    {
        for (const std::string_view text: {"inf", "INF", "Inf", "-inf", "infinity"})
        {
            const std::optional<double> parsed = withRawText(text, parseDoubleText);
            ASSERT_TRUE(parsed.has_value()) << text;
            // 用 isinf 加符号判定而不是 isposinf：后者在 C++ 里是可选设施，MSVC 不提供
            const bool positive = text.front() != '-';
            EXPECT_TRUE(std::isinf(*parsed) && (*parsed > 0.0) == positive) << text;
        }

        for (const std::string_view text: {"nan", "NAN", "NaN", "-nan"})
        {
            const std::optional<double> parsed = withRawText(text, parseDoubleText);
            ASSERT_TRUE(parsed.has_value()) << text;
            EXPECT_TRUE(std::isnan(*parsed)) << text;
        }
    }

    /**
     * @brief 验证非有限值的近似写法不被接受
     * @details 认出这几种拼写靠的是标准文法，不是模糊匹配：被截断、多出字母或带前后空白的写法都不算，
     *          而前导 '+' 与其它浮点数值同样不被接受。
     */
    TEST(MySqlValueConversionDouble, DoesNotGuessFromNearMissNonFiniteSpellings)
    {
        for (const std::string_view text: {"in", "infi", "info", "nanx", "infinityx", " inf", "inf ", "nan\t", "+inf", "+-inf", "n a n"})
        {
            EXPECT_FALSE(withRawText(text, parseDoubleText).has_value()) << "\"" << text << "\" 不是可接受的浮点文本";
        }
    }

    /**
     * @brief 验证二进制列的判定只看字符集，而不是只看类型码
     * @details MySQL 的 BLOB 与 TEXT 在协议层共用 MYSQL_TYPE_BLOB，VARBINARY 与 VARCHAR 同理；
     *          只看类型码会把 TEXT 读成字节（字符集解释丢失），只看字符集又会把整数等列也当字节。
     */
    TEST(MySqlValueConversionBinary, RecognizesBinaryOnlyByCharacterSetAmongByteTypedColumns)
    {
        EXPECT_TRUE(isBinaryColumn(MYSQL_TYPE_BLOB, kBinaryCharacterSetNumber));
        EXPECT_TRUE(isBinaryColumn(MYSQL_TYPE_TINY_BLOB, kBinaryCharacterSetNumber));
        EXPECT_TRUE(isBinaryColumn(MYSQL_TYPE_VAR_STRING, kBinaryCharacterSetNumber));

        // 类型码相同、字符集不是 binary ⇒ 文本列
        EXPECT_FALSE(isBinaryColumn(MYSQL_TYPE_BLOB, kTextCharacterSetNumber));
        EXPECT_FALSE(isBinaryColumn(MYSQL_TYPE_VAR_STRING, kTextCharacterSetNumber));
        // 字符集是 binary 但类型不是字节序列的列，也不按二进制交出
        EXPECT_FALSE(isBinaryColumn(MYSQL_TYPE_LONGLONG, kBinaryCharacterSetNumber));
    }

    /**
     * @brief 验证 convertColumnText 的类型映射与「零长 BLOB ≠ NULL」这条区分
     * @details 内嵌 '\0' 必须按长度保留：按零终止取字节会把二进制载荷截断成半段。
     */
    TEST(MySqlValueConversionColumn, MapsDeclaredTypeAndKeepsBlobBytesDistinctFromNull)
    {
        const std::string_view nine{"-9223372036854775808"};
        const DatabaseValue    integerValue = convertColumnText(MYSQL_TYPE_LONGLONG, kTextCharacterSetNumber, nine.data(), nine.size());
        EXPECT_EQ(std::get<std::int64_t>(integerValue), std::numeric_limits<std::int64_t>::min());

        const std::string_view positiveInfinity{"inf"};
        const DatabaseValue    realValue = convertColumnText(MYSQL_TYPE_DOUBLE, kTextCharacterSetNumber, positiveInfinity.data(), positiveInfinity.size());
        ASSERT_TRUE(std::holds_alternative<double>(realValue)) << databaseValueTypeName(realValue);
        EXPECT_TRUE(std::isinf(std::get<double>(realValue)) && std::get<double>(realValue) > 0.0);

        // DECIMAL 是精确小数的常规载体：转 double 会在末位丢精度且不可逆，因此交原文
        const std::string_view decimal{"12345678901234567.89"};
        const DatabaseValue    decimalValue = convertColumnText(MYSQL_TYPE_NEWDECIMAL, kTextCharacterSetNumber, decimal.data(), decimal.size());
        EXPECT_EQ(std::get<std::string>(decimalValue), "12345678901234567.89");

        // 内嵌 '\0' 的字节序列原样保留；零长 BLOB 是「有值且为空」，不是 SQL NULL
        const std::string   blob("ab\0cd", 5);
        const DatabaseValue binaryValue = convertColumnText(MYSQL_TYPE_BLOB, kBinaryCharacterSetNumber, blob.data(), blob.size());
        ASSERT_TRUE(std::holds_alternative<BinaryBytes>(binaryValue)) << databaseValueTypeName(binaryValue);
        ASSERT_EQ(std::get<BinaryBytes>(binaryValue).size(), 5U);
        EXPECT_EQ(std::get<BinaryBytes>(binaryValue)[2], std::uint8_t{0});

        const DatabaseValue emptyBlob = convertColumnText(MYSQL_TYPE_BLOB, kBinaryCharacterSetNumber, blob.data(), 0U);
        ASSERT_TRUE(std::holds_alternative<BinaryBytes>(emptyBlob)) << databaseValueTypeName(emptyBlob);
        EXPECT_TRUE(std::get<BinaryBytes>(emptyBlob).empty());
    }

    /**
     * @brief 钉住 BIT 列按大端整数还原，而不是把一串控制字节冒充成文本列
     * @details 真机实测的线格式：BIT(8) 的 200 是 1 字节 0xC8，BIT(1) 的 0 是 1 字节 NUL。
     *          按文本交出时 ORM 侧只能报「文本无法映射到整型」，而 BIT(1)=0 更是一串看着像空串的内容。
     */
    TEST(MySqlValueConversion, BitColumnsBecomeIntegers)
    {
        const std::string   bit8Value = std::string("\xC8", 1);
        const DatabaseValue bit8      = convertColumnText(MYSQL_TYPE_BIT, kBinaryCharacterSetNumber, bit8Value.data(), bit8Value.size());
        ASSERT_TRUE(std::holds_alternative<std::int64_t>(bit8)) << databaseValueTypeName(bit8);
        EXPECT_EQ(std::get<std::int64_t>(bit8), 200) << "字节被符号扩展或字节序读反都会得到别的数";

        const std::string bit1Set   = std::string("\x01", 1);
        const std::string bit1Clear = std::string("\x00", 1);
        EXPECT_EQ(std::get<std::int64_t>(convertColumnText(MYSQL_TYPE_BIT, kBinaryCharacterSetNumber, bit1Set.data(), bit1Set.size())), 1);
        // 清位的 BIT(1) 必须是整数 0：交出长度 1 的 NUL 文本时，它在调用方眼里与「空串」几乎没区别
        EXPECT_EQ(std::get<std::int64_t>(convertColumnText(MYSQL_TYPE_BIT, kBinaryCharacterSetNumber, bit1Clear.data(), bit1Clear.size())), 0);

        // 多字节按大端拼装：先到的 0x12 是高位
        const std::string bit16Value = std::string("\x12\x34", 2);
        EXPECT_EQ(std::get<std::int64_t>(convertColumnText(MYSQL_TYPE_BIT, kBinaryCharacterSetNumber, bit16Value.data(), bit16Value.size())), 0x1234);
    }

    /**
     * @brief 钉住 BIT(64) 超出 int64 的那半段按十进制文本交出，不凭空造数
     * @details 与 BIGINT UNSIGNED 同一条约定：RowMapper 的文本支路能把十进制文本原样读回无符号成员。
     */
    TEST(MySqlValueConversion, Bit64BeyondInt64RangeBecomesDecimalText)
    {
        const std::string   allOnes       = std::string("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8);
        const DatabaseValue overflowValue = convertColumnText(MYSQL_TYPE_BIT, kBinaryCharacterSetNumber, allOnes.data(), allOnes.size());
        ASSERT_TRUE(std::holds_alternative<std::string>(overflowValue)) << databaseValueTypeName(overflowValue);
        EXPECT_EQ(std::get<std::string>(overflowValue), "18446744073709551615");

        // 恰好落在 int64 上界时仍按整数交出：分界线两侧不能都退化
        const std::string int64Maximum = std::string("\x7F\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8);
        EXPECT_EQ(std::get<std::int64_t>(convertColumnText(MYSQL_TYPE_BIT, kBinaryCharacterSetNumber, int64Maximum.data(), int64Maximum.size())),
                  std::numeric_limits<std::int64_t>::max());
    }

    /**
     * @brief 钉住 BIT 的意外宽度一律退回原文：宁可给回原始字节也不猜一个数
     * @details 服务端不会给出长度为 0 或超过 8 字节的 BIT 载荷；真遇到（协议错位、将来加宽）说明这份
     *          映射的前提已不成立，此时取整或截断到 64 位都属静默变形。
     */
    TEST(MySqlValueConversion, UnexpectedBitPayloadWidthsStayVerbatim)
    {
        const std::string   emptyPayload = std::string();
        const DatabaseValue emptyValue   = convertColumnText(MYSQL_TYPE_BIT, kBinaryCharacterSetNumber, emptyPayload.data(), emptyPayload.size());
        ASSERT_TRUE(std::holds_alternative<std::string>(emptyValue)) << "空载荷应退回空文本，而不是 monostate（那等于说它是 NULL）";
        EXPECT_TRUE(std::get<std::string>(emptyValue).empty());

        const std::string   nineBytes    = std::string("123456789", 9);
        const DatabaseValue tooWideValue = convertColumnText(MYSQL_TYPE_BIT, kBinaryCharacterSetNumber, nineBytes.data(), nineBytes.size());
        ASSERT_TRUE(std::holds_alternative<std::string>(tooWideValue)) << databaseValueTypeName(tooWideValue);
        EXPECT_EQ(std::get<std::string>(tooWideValue), nineBytes);
    }

} // namespace AsynGyanis::Database::Detail

#else

namespace AsynGyanis::Database::Detail
{
    /**
     * @brief 未编译 MySQL 驱动时的占位用例
     * @details 被量实现要看到真实 mysql.h 才能拿到 enum_field_types 常量，这里不重复声明那些枚举的数值；
     *          保留一条 SKIP 用例是为了让「本文件在当前配置下没测东西」在报告里可见。
     */
    TEST(MySqlValueConversion, SkippedWhenMySQLDriverIsNotBuilt)
    {
        GTEST_SKIP() << "当前构建未编译 MySQL 驱动（未定义 DATABASE_HAS_MYSQL），"
                     << "MySqlValueConversion 的直测随驱动一起开关";
    }

} // namespace AsynGyanis::Database::Detail

#endif
