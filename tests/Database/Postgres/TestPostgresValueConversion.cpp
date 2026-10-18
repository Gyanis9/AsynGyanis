// PostgresValueConversion 的 BYTEA 那一半直测：编码与解码都是纯函数，不碰任何句柄，
// 因此「二进制列换引擎就变语义」这条跨引擎陷阱在离线配置下就能钉死。整份按
// DATABASE_HAS_POSTGRES 门控：被量头带着 libpq-fe.h（要 Oid 与 PQftype 的取值域），
// 未编译 PostgreSQL 驱动的配置里本文件只留一条 SKIP 用例，不去猜那些 OID 的数值。
//
// 覆盖场景：
// - toPostgresByteaText：'\x' + 小写十六进制；零长载荷只剩前缀；逐字节（含 0x00 与 0xFF）都能解回原值
// - parsePostgresByteaText：十六进制形（大小写都收）与转义形（老服务端或 bytea_output=escape 给的那一系）
//   同解；写坏的十六进制形（奇数位、非十六进制数字、八进制越界或凑不满三位）一律拒、交回调用方按原文
//   处理，不猜半个字节；不带反斜杠的文本是合法的转义形，按字面字节收而不是拒
// - toPostgresTextParameter：BinaryBytes 走 BYTEA 文本参数，不再是「容器参数被拒」
// - convertColumnText：OID 17 交回 BinaryBytes；解不开的形态退回原文，与其余列的兜底口径一致

#include "Database/Common/BinaryBytes.h"
#include "Database/Common/DatabaseValue.h"
#include "ExpectedAssertions.h"

#include <gtest/gtest.h>

#ifdef DATABASE_HAS_POSTGRES

// 被量头文件带着 libpq-fe.h（Oid 与类型标识来自它），因此 include 必须落在门控里面
#include "Database/Postgres/PostgresValueConversion.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace AsynGyanis::Database::Detail
{
    namespace
    {
        /// 三家同一条判据用的载荷：0x00（内嵌 NUL）、0x5C（反斜杠，转义形的关键）、0xFF（高位）、
        /// 0x41（可打印）、再加「你好」的 UTF-8 序列——多字节序列用来证明编码不认「字符」只认字节
        const BinaryBytes kSharedBinaryPayload = {0x00, 0x5C, 0xFF, 0x41, 0xE4, 0xBD, 0xA0, 0xE5, 0xA5, 0xBD, 0x00};

        /**
         * @brief 把文本按字面长度包成 string_view，省得在判据里再数一遍
         * @param text 字面量
         * @return std::string_view 去掉结尾零终止符的视图
         */
        std::string_view literalView(const char *const text)
        {
            return std::string_view{text, text ? std::char_traits<char>::length(text) : 0U};
        }
    } // namespace

    // ------------------------------------------------------------------------
    // 编码：送出方向
    // ------------------------------------------------------------------------

    /**
     * @brief 钉住 BYTEA 的文本输入形态：'\x' 前缀加小写十六进制
     * @note 前缀是小写 '\x' 而数字是小写十六进制——服务端 byteain() 两种大小写都收，
     *       但本库要交出的文本得是定死的（判据要逐字符比，不能随实现改写法）
     */
    TEST(PostgresBytea, EncodesPayloadAsLowercaseHexWithPrefix)
    {
        EXPECT_EQ(literalView("\\x005c41"), toPostgresByteaText(BinaryBytes{0x00, 0x5C, 0x41}));
        EXPECT_EQ(literalView("\\xe4bda0"), toPostgresByteaText(BinaryBytes{0xE4, 0xBD, 0xA0}));
    }

    /// @brief 零长载荷编成只剩前缀：那是「长度为 0 的 BYTEA」，与 SQL NULL 是两件事
    TEST(PostgresBytea, EmptyPayloadEncodesToPrefixOnly)
    {
        EXPECT_EQ(literalView("\\x"), toPostgresByteaText(BinaryBytes{}));
    }

    /**
     * @brief 逐字节（0..255）编出去再解回来必须是原值
     * @details 0x00 与 0xFF 是文本搬运最容易断的两格：前者被当零终止符、后者被按字符集重编码。
     *          这一条把「任意字节都能过」钉成机械事实，而不是挑几个样本。
     */
    TEST(PostgresBytea, EveryByteValueSurvivesEncodeThenDecode)
    {
        BinaryBytes allBytes;
        allBytes.reserve(256U);
        for (int value = 0; value < 256; ++value)
        {
            allBytes.push_back(static_cast<std::uint8_t>(value));
        }

        const std::string                encoded = toPostgresByteaText(allBytes);
        const std::optional<BinaryBytes> decoded = parsePostgresByteaText(encoded.data(), encoded.size());

        ASSERT_HAS_VALUE(decoded);
        EXPECT_EQ(allBytes, *decoded);
    }

    // ------------------------------------------------------------------------
    // 解码：读入方向
    // ------------------------------------------------------------------------

    /// @brief 十六进制形两种大小写都认（服务端的 bytea_output 恒给小写，但手工写入的文本可能是大写）
    TEST(PostgresBytea, DecodesHexFormInEitherCase)
    {
        EXPECT_EQ(BinaryBytes({0x00, 0x5C, 0xFF, 0x41}), parsePostgresByteaText("\\x005cff41", 10).value());
        EXPECT_EQ(BinaryBytes({0x00, 0x5C, 0xFF, 0x41}), parsePostgresByteaText("\\x005CFF41", 10).value());
        EXPECT_TRUE(parsePostgresByteaText("\\x", 2).value().empty());
    }

    /**
     * @brief 钉住转义形（老默认，或会话把 bytea_output 设成 escape 时给出的那一系）
     * @details 反斜杠本身写成 '\\'，不可打印字节写成三位八进制。这一形必须与十六进制形同解，
     *          否则换一台把 GUC 设成 escape 的服务端，同一列就读成另一种东西。
     */
    TEST(PostgresBytea, DecodesEscapeFormLikeOlderServerOutput)
    {
        // "\000\377A" 逐字节是 0x00、0xFF、0x41（九个字符：反斜杠加三位八进制两组，再加字面 A）
        EXPECT_EQ(BinaryBytes({0x00, 0xFF, 0x41}), parsePostgresByteaText("\\000\\377A", 9).value());
        // "\\" 是一个反斜杠本身，不是八进制数的开头
        EXPECT_EQ(BinaryBytes({0x5C}), parsePostgresByteaText("\\\\", 2).value());
    }

    /**
     * @brief 十六进制形写坏的一律拒，而转义形的字面字节不算写坏
     * @details 奇数位的十六进制、前缀后跟非十六进制字符、三位八进制越界（\400）、八进制凑不满三位
     *          都属「写坏」。反过来，一串不带反斜杠的文本是合法的转义形（可打印字符按字面收），
     *          拒掉它就是把 escape 输出的 BYTEA 读成文本——这一格刻意钉在反方向上。
     *          拒掉之后调用方退回原文（见 convertColumnText 的兜底），而不是猜一个字节序列。
     */
    TEST(PostgresBytea, RejectsTextThatIsNeitherHexNorEscape)
    {
        EXPECT_FALSE(parsePostgresByteaText("\\x005", 5).has_value()); // 奇数位
        EXPECT_FALSE(parsePostgresByteaText("\\xzz", 4).has_value());  // 非十六进制数字
        EXPECT_FALSE(parsePostgresByteaText("\\400", 4).has_value());  // 八进制越界
        EXPECT_FALSE(parsePostgresByteaText("\\0", 2).has_value());    // 八进制不满三位

        // 没有前缀 ≠ 写坏了：这是转义形的常态写法，'x','0','0','5','c' 五个字面字节
        EXPECT_EQ(BinaryBytes({0x78, 0x30, 0x30, 0x35, 0x63}), parsePostgresByteaText("x005c", 5).value());
    }

    // ------------------------------------------------------------------------
    // 送出方向：BinaryBytes 现在是可绑定的取值
    // ------------------------------------------------------------------------

    /// @brief 二进制取值编成 BYTEA 的文本参数：绑定层自己编码，调用方无感
    TEST(PostgresTextParameter, BinaryValueBecomesByteaHexTextParameter)
    {
        const std::optional<PostgresTextParameter> parameter = toPostgresTextParameter(DatabaseValue{BinaryBytes{0x00, 0x5C, 0xFF}});

        ASSERT_HAS_VALUE(parameter);
        EXPECT_FALSE(parameter->isNull);
        EXPECT_EQ(literalView("\\x005cff"), parameter->text);
    }

    /// @brief 编码不丢内嵌 NUL：参数文本本身是纯 ASCII，长度由调用方交给 libpq
    TEST(PostgresTextParameter, BinaryEncodingKeepsEveryByteOfTheSharedPayload)
    {
        const std::optional<PostgresTextParameter> parameter = toPostgresTextParameter(DatabaseValue{kSharedBinaryPayload});

        ASSERT_HAS_VALUE(parameter);
        const std::optional<BinaryBytes> decoded = parsePostgresByteaText(parameter->text.data(), parameter->text.size());
        ASSERT_HAS_VALUE(decoded);
        EXPECT_EQ(kSharedBinaryPayload, *decoded);
    }

    // ------------------------------------------------------------------------
    // 读入方向：BYTEA 列现在是二进制取值
    // ------------------------------------------------------------------------

    /// @brief OID 17 交回 BinaryBytes 而不是十六进制文本：这一条是「三家同解」在 PG 侧的落点
    TEST(PostgresColumnConversion, ByteaColumnTextBecomesBinaryAlternative)
    {
        const char hexText[] = {'\\', 'x', '0', '0', '5', 'c', 'f', 'f', '4', '1'};

        const DatabaseValue value = convertColumnText(TypeOid::kBytea, hexText, sizeof(hexText));
        const auto         *bytes = std::get_if<BinaryBytes>(&value);

        ASSERT_TRUE(bytes != nullptr) << "BYTEA 列没被解成二进制取值";
        EXPECT_EQ(BinaryBytes({0x00, 0x5C, 0xFF, 0x41}), *bytes);
    }

    /// @brief 转义形的列值同样解得开：服务端把 bytea_output 设成 escape 时读到的就是这个形状
    TEST(PostgresColumnConversion, ByteaColumnInEscapeFormAlsoBecomesBinary)
    {
        const char escapeText[] = {'\\', '0', '0', '0', '\\', '3', '7', '7', 'A'};

        const DatabaseValue value = convertColumnText(TypeOid::kBytea, escapeText, sizeof(escapeText));
        const auto         *bytes = std::get_if<BinaryBytes>(&value);

        ASSERT_TRUE(bytes != nullptr);
        EXPECT_EQ(BinaryBytes({0x00, 0xFF, 0x41}), *bytes);
    }

    /**
     * @brief 解不开的 BYTEA 文本退回原文，不猜字节
     * @details 与其余列的兜底口径一致（整数列读出 "12abc" 也交原文）。空列值按零长二进制收：
     *          那是「长度为 0 的 BYTEA」，与 SQL NULL 由调用方用 PQgetisnull 区分，不在这里混淆。
     */
    TEST(PostgresColumnConversion, UndecodableByteaTextFallsBackToRawText)
    {
        const std::string_view malformed{"\\x0"};

        const DatabaseValue value = convertColumnText(TypeOid::kBytea, malformed.data(), malformed.size());
        ASSERT_TRUE(std::holds_alternative<std::string>(value));
        EXPECT_EQ("\\x0", std::get<std::string>(value));
    }

    /// @brief 零长的列值是一段零长二进制，不是 NULL 也不是「解不开」
    TEST(PostgresColumnConversion, ZeroLengthByteaIsBinaryNotEmptyText)
    {
        const char prefixOnly[] = {'\\', 'x'};

        const DatabaseValue value = convertColumnText(TypeOid::kBytea, prefixOnly, sizeof(prefixOnly));
        const auto         *bytes = std::get_if<BinaryBytes>(&value);

        ASSERT_TRUE(bytes != nullptr);
        EXPECT_TRUE(bytes->empty());
    }

    /// @brief 文本列的取值路径不受影响：TEXTOID（25）仍按原文交出，即便内容长得像十六进制
    TEST(PostgresColumnConversion, TextColumnCarryingHexLookingContentStaysText)
    {
        constexpr Oid kTextOid = 25U; ///< TEXTOID：本头没列它（读侧不需要区分文本家族），这里按 pg_type 的既定值用

        const std::string_view looksLikeBytea{"\\x005cff"};

        const DatabaseValue value = convertColumnText(kTextOid, looksLikeBytea.data(), looksLikeBytea.size());
        ASSERT_TRUE(std::holds_alternative<std::string>(value)) << "文本列被当成二进制解了";
        EXPECT_EQ("\\x005cff", std::get<std::string>(value));
    }

} // namespace AsynGyanis::Database::Detail

#else

TEST(PostgresValueConversion, SkippedWithoutLibpq)
{
    GTEST_SKIP() << "当前构建未找到 libpq，PostgresValueConversion 的 BYTEA 直测无从谈起";
}

#endif // DATABASE_HAS_POSTGRES
