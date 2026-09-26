#include "Net/Http3/Http3HeaderValidation.h"

#include <gtest/gtest.h>

#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using AsynGyanis::Net::Http3HeaderError;
    using AsynGyanis::Net::Http3HeaderErrorKind;
    using AsynGyanis::Net::Http3HeaderValidator;
    using AsynGyanis::Net::Http3MessageKind;
    using AsynGyanis::Net::toHttp3ErrorCode;

    /// 一条最小合法请求的头段：绝大多数用例在它基础上改一处，差异只来自被改的那条规则
    const std::vector<std::pair<std::string_view, std::string_view>> &minimalRequestFields()
    {
        static const std::vector<std::pair<std::string_view, std::string_view>> kFields = {
                {":method", "GET"}, {":scheme", "https"}, {":authority", "example.com"}, {":path", "/json"}, {"accept", "*/*"},
        };
        return kFields;
    }

    /**
     * @brief 按序把字段喂进判定器，最后收尾
     * @details 返回第一个失败（含 endHeaderBlock 的判定），全通过则返回空。用例只关心「有没有被拒」时用
     *          它，关心「被哪条规则拒」时用 feedExpectingError。
     */
    std::expected<void, Http3HeaderError> feedRequest(Http3HeaderValidator &validator, const std::vector<std::pair<std::string_view, std::string_view>> &fields)
    {
        if (auto result = validator.beginHeaderBlock(false); !result)
        {
            return result;
        }
        for (const auto &[name, value]: fields)
        {
            if (auto result = validator.onHeaderField(name, value); !result)
            {
                return result;
            }
        }
        return validator.endHeaderBlock();
    }

    /// 把最小合法请求的第 index 个字段换掉，其余照旧
    std::vector<std::pair<std::string_view, std::string_view>> requestWithFieldReplaced(const std::size_t index, const std::string_view name, const std::string_view value)
    {
        auto fields   = minimalRequestFields();
        fields[index] = {name, value};
        return fields;
    }

    /// 在最小合法请求上追加一个字段
    std::vector<std::pair<std::string_view, std::string_view>> requestWithExtraField(const std::string_view name, const std::string_view value)
    {
        auto fields = minimalRequestFields();
        fields.emplace_back(name, value);
        return fields;
    }

    /// 断言被拒，且类别正是 expected 的那一个：只看「失败」会把「字段名非法」和「缺伪头」混为一谈
    void expectRejected(const std::expected<void, Http3HeaderError> &result, const Http3HeaderErrorKind expectedKind, std::string_view context)
    {
        ASSERT_FALSE(result.has_value()) << context << "：本应被拒却通过了";
        EXPECT_EQ(result.error().kind, expectedKind) << context << "：失败类别不对，实际文案=" << result.error().message;
        EXPECT_FALSE(result.error().message.empty()) << context << "：失败文案为空，日志里无从定位";
    }
} // namespace

TEST(Http3HeaderValidation, MinimalRequestHeadPasses)
{
    Http3HeaderValidator validator(Http3MessageKind::Request);
    const auto           result = feedRequest(validator, minimalRequestFields());
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // 伪头要原样交给上层：路由按 :path、host 补齐按 :authority，判定器不做任何加工
    EXPECT_EQ(validator.methodText(), "GET");
    EXPECT_EQ(validator.schemeText(), "https");
    EXPECT_EQ(validator.authorityText(), "example.com");
    EXPECT_EQ(validator.pathText(), "/json");
    EXPECT_TRUE(validator.protocolText().empty());
    EXPECT_FALSE(validator.hasHostHeader());
    EXPECT_FALSE(validator.contentLengthByteCount().has_value());
}

TEST(Http3HeaderValidation, EveryHeaderErrorMapsToMessageError)
{
    // 钉住 §4.1.2 的统一处置：这一类畸形不分码，全部 H3_MESSAGE_ERROR。
    // 若哪天有人给某条规则分出一个「只回 400 不重置流」的码，这条用例会红，逼他去看规范
    EXPECT_EQ(static_cast<std::uint64_t>(toHttp3ErrorCode(Http3HeaderErrorKind::UppercaseFieldName)), 0x010EULL);
    EXPECT_EQ(toHttp3ErrorCode(Http3HeaderErrorKind::ContentLengthConflict), toHttp3ErrorCode(Http3HeaderErrorKind::EmptyPath));
}

TEST(Http3HeaderValidation, MissingRequiredPseudoHeaderIsRejected)
{
    // 逐个删掉一个必填伪头：三条都得被拒，缺哪个报哪个
    Http3HeaderValidator validator(Http3MessageKind::Request);
    expectRejected(feedRequest(validator, {{":scheme", "https"}, {":authority", "example.com"}, {":path", "/"}}), Http3HeaderErrorKind::MissingPseudoHeader, "缺 :method");

    Http3HeaderValidator schemeMissing(Http3MessageKind::Request);
    expectRejected(feedRequest(schemeMissing, {{":method", "GET"}, {":authority", "example.com"}, {":path", "/"}}), Http3HeaderErrorKind::MissingPseudoHeader, "缺 :scheme");

    Http3HeaderValidator pathMissing(Http3MessageKind::Request);
    expectRejected(feedRequest(pathMissing, {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.com"}}), Http3HeaderErrorKind::MissingPseudoHeader, "缺 :path");
}

TEST(Http3HeaderValidation, DuplicatePseudoHeaderIsRejected)
{
    // 重复的伪头必须紧跟在同类伪头之后出现才测得准：把它放到 accept 之后，先撞上的是
    // 「伪头不得排在普通字段之后」那条规则（§4.3），测不到重复判定本身
    Http3HeaderValidator validator(Http3MessageKind::Request);
    expectRejected(feedRequest(validator, {{":method", "GET"}, {":method", "POST"}, {":scheme", "https"}, {":authority", "example.com"}, {":path", "/"}}),
                   Http3HeaderErrorKind::DuplicatePseudoHeader, "两个 :method");

    Http3HeaderValidator schemeDuplicated(Http3MessageKind::Request);
    expectRejected(feedRequest(schemeDuplicated, {{":method", "GET"}, {":scheme", "https"}, {":scheme", "http"}, {":authority", "example.com"}, {":path", "/"}}),
                   Http3HeaderErrorKind::DuplicatePseudoHeader, "两个 :scheme");

    Http3HeaderValidator pathDuplicated(Http3MessageKind::Request);
    expectRejected(feedRequest(pathDuplicated, {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.com"}, {":path", "/a"}, {":path", "/b"}}),
                   Http3HeaderErrorKind::DuplicatePseudoHeader, "两个 :path");

    Http3HeaderValidator authorityDuplicated(Http3MessageKind::Request);
    expectRejected(feedRequest(authorityDuplicated, {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.com"}, {":authority", "other.example"}, {":path", "/"}}),
                   Http3HeaderErrorKind::DuplicatePseudoHeader, "两个 :authority");
}

TEST(Http3HeaderValidation, UndefinedPseudoHeaderIsRejected)
{
    // 三个用例都把待测伪头排在普通字段之前：放到末尾会先撞上「伪头不得排在普通字段之后」
    Http3HeaderValidator validator(Http3MessageKind::Request);
    // :protocol 未经对端许可即未定义
    expectRejected(feedRequest(validator, {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.com"}, {":path", "/"}, {":protocol", "websocket"}}),
                   Http3HeaderErrorKind::UndefinedPseudoHeader, "未开扩展 CONNECT 却带 :protocol");

    Http3HeaderValidator statusInRequest(Http3MessageKind::Request);
    expectRejected(feedRequest(statusInRequest, {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.com"}, {":path", "/"}, {":status", "200"}}),
                   Http3HeaderErrorKind::UndefinedPseudoHeader, "请求里出现 :status");

    Http3HeaderValidator colonOnly(Http3MessageKind::Request);
    expectRejected(feedRequest(colonOnly, {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.com"}, {":path", "/"}, {":", "x"}}),
                   Http3HeaderErrorKind::UndefinedPseudoHeader, "伪头名只剩一个冒号");

    Http3HeaderValidator uppercasePseudo(Http3MessageKind::Request);
    expectRejected(feedRequest(uppercasePseudo, {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.com"}, {":path", "/"}, {":Method", "POST"}}),
                   Http3HeaderErrorKind::UndefinedPseudoHeader, "大小写不同的伪头名不得被当成 :method 的第二次出现");
}

TEST(Http3HeaderValidation, PseudoHeaderAfterRegularFieldIsRejected)
{
    // §4.3：伪头必须全部排在普通字段之前。这里 accept 已经在前，再来的 :scheme 必须被打死
    Http3HeaderValidator                                             validator(Http3MessageKind::Request);
    const std::vector<std::pair<std::string_view, std::string_view>> fields = {
            {":method", "GET"}, {"accept", "*/*"}, {":scheme", "https"}, {":authority", "example.com"}, {":path", "/"},
    };
    expectRejected(feedRequest(validator, fields), Http3HeaderErrorKind::PseudoHeaderAfterField, "普通字段之后的伪头");
}

TEST(Http3HeaderValidation, IllegalFieldNamesAreRejected)
{
    Http3HeaderValidator uppercase(Http3MessageKind::Request);
    expectRejected(feedRequest(uppercase, requestWithFieldReplaced(4, "Accept", "*/*")), Http3HeaderErrorKind::UppercaseFieldName, "含大写的字段名（§4.2 要求编码前转小写）");

    Http3HeaderValidator spaceInName(Http3MessageKind::Request);
    expectRejected(feedRequest(spaceInName, requestWithFieldReplaced(4, "acce pt", "*/*")), Http3HeaderErrorKind::IllegalFieldNameCharacter, "含空格的字段名");

    Http3HeaderValidator emptyName(Http3MessageKind::Request);
    expectRejected(feedRequest(emptyName, requestWithFieldReplaced(4, "", "v")), Http3HeaderErrorKind::EmptyFieldName, "空字段名");

    Http3HeaderValidator nulInName(Http3MessageKind::Request);
    expectRejected(feedRequest(nulInName, requestWithFieldReplaced(4, std::string_view("acc\0ept", 6), "*/*")), Http3HeaderErrorKind::IllegalFieldNameCharacter, "含 NUL 的字段名");
}

TEST(Http3HeaderValidation, ControlCharactersInFieldValuesAreRejected)
{
    // CR/LF/NUL 是响应拆分与头部截断的最小形态，必须在协议层就挡住，不能留给业务
    for (const char forbidden: {static_cast<char>(0x0D), static_cast<char>(0x0A), static_cast<char>(0x00), static_cast<char>(0x7F)})
    {
        Http3HeaderValidator validator(Http3MessageKind::Request);
        const std::string    valueWithForbidden = std::string("*/*") + forbidden + "x";
        const auto           result             = feedRequest(validator, requestWithFieldReplaced(4, "accept", valueWithForbidden));
        expectRejected(result, Http3HeaderErrorKind::IllegalFieldValueCharacter, "字段值含控制字符");
    }
}

TEST(Http3HeaderValidation, HorizontalTabAndObsTextInValuesAreAccepted)
{
    // HTAB 与 obs-text（0x80-0xFF）是 RFC 9110 §5.6.2 允许的：一并拒掉会打死合法的非 ASCII 头值
    Http3HeaderValidator withTab(Http3MessageKind::Request);
    EXPECT_TRUE(feedRequest(withTab, requestWithFieldReplaced(4, "accept", "a\tb")).has_value());

    Http3HeaderValidator withObsText(Http3MessageKind::Request);
    EXPECT_TRUE(feedRequest(withObsText, requestWithFieldReplaced(4, "x-note", std::string("\xE4\xB8\xAD\xE6\x96\x87", 6))).has_value()) << "UTF-8 头值应原样收下";
}

TEST(Http3HeaderValidation, ConnectionSpecificFieldsAreRejectedButTeTrailersIsAllowed)
{
    for (const std::string_view forbidden: {"connection", "keep-alive", "proxy-connection", "transfer-encoding", "upgrade"})
    {
        Http3HeaderValidator validator(Http3MessageKind::Request);
        expectRejected(feedRequest(validator, requestWithExtraField(forbidden, "x")), Http3HeaderErrorKind::ConnectionFieldProhibited, forbidden);
    }

    // §4.2 唯一的例外：TE 允许，但只允许 trailers 这一个值
    Http3HeaderValidator teTrailers(Http3MessageKind::Request);
    EXPECT_TRUE(feedRequest(teTrailers, requestWithExtraField("te", "trailers")).has_value()) << "TE: trailers 应当合法";

    Http3HeaderValidator teOther(Http3MessageKind::Request);
    expectRejected(feedRequest(teOther, requestWithExtraField("te", "chunked")), Http3HeaderErrorKind::TeValueNotAllowed, "TE 的其他取值");

    Http3HeaderValidator tePadded(Http3MessageKind::Request);
    EXPECT_TRUE(feedRequest(tePadded, requestWithExtraField("te", " trailers ")).has_value()) << "TE 两侧的空白按 OWS 处理";
}

TEST(Http3HeaderValidation, ContentLengthMustBeASingleConsistentNumber)
{
    for (const std::string_view invalid: {"abc", "3, 3", "-1", "1e3", "0x10", " 12x"})
    {
        Http3HeaderValidator validator(Http3MessageKind::Request);
        expectRejected(feedRequest(validator, requestWithFieldReplaced(4, "content-length", invalid)), Http3HeaderErrorKind::ContentLengthInvalid, invalid);
    }

    Http3HeaderValidator sameTwice(Http3MessageKind::Request);
    const auto           sameResult =
            feedRequest(sameTwice, {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.com"}, {":path", "/"}, {"content-length", "4"}, {"content-length", "4"}});
    EXPECT_TRUE(sameResult.has_value()) << sameResult.error().message << "；重复但取值一致的 content-length 应接受";
    ASSERT_TRUE(sameTwice.contentLengthByteCount().has_value());
    EXPECT_EQ(*sameTwice.contentLengthByteCount(), 4ULL);

    Http3HeaderValidator conflict(Http3MessageKind::Request);
    expectRejected(
            feedRequest(conflict, {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.com"}, {":path", "/"}, {"content-length", "4"}, {"content-length", "5"}}),
            Http3HeaderErrorKind::ContentLengthConflict, "取值不一致的第二个 content-length");
}

TEST(Http3HeaderValidation, TrailersSectionHasItsOwnRules)
{
    Http3HeaderValidator validator(Http3MessageKind::Request);
    ASSERT_TRUE(feedRequest(validator, minimalRequestFields()).has_value());

    // 尾段：普通字段合法，伪头与被禁字段非法（§4.3、RFC 9110 §6.5）
    ASSERT_TRUE(validator.beginHeaderBlock(true).has_value());
    EXPECT_TRUE(validator.onHeaderField("x-checksum", "abc123").has_value());
    expectRejected(validator.onHeaderField(":method", "GET"), Http3HeaderErrorKind::PseudoHeaderInTrailers, "尾段里的伪头");
    expectRejected(validator.onHeaderField("content-length", "4"), Http3HeaderErrorKind::ProhibitedFieldInTrailers, "尾段里的 content-length");
    expectRejected(validator.onHeaderField("host", "example.com"), Http3HeaderErrorKind::ProhibitedFieldInTrailers, "尾段里的 host");
    EXPECT_TRUE(validator.endHeaderBlock().has_value());
}

TEST(Http3HeaderValidation, SecondHeadSectionWithoutTrailersIsRejected)
{
    Http3HeaderValidator validator(Http3MessageKind::Request);
    ASSERT_TRUE(feedRequest(validator, minimalRequestFields()).has_value());
    // 头段之后只能再来一个尾段；再来一个头段属于非法的消息序列
    expectRejected(validator.beginHeaderBlock(false), Http3HeaderErrorKind::InvalidMessageSequence, "第二个头段");

    Http3HeaderValidator trailingFirst(Http3MessageKind::Request);
    expectRejected(trailingFirst.beginHeaderBlock(true), Http3HeaderErrorKind::InvalidMessageSequence, "头段之前就来尾段");
}

TEST(Http3HeaderValidation, ClassicConnectOmitsSchemeAndPath)
{
    // §4.4：CONNECT 必须省掉 :scheme 与 :path，且必须给 :authority
    Http3HeaderValidator plainConnect(Http3MessageKind::Request);
    const auto           okResult = feedRequest(plainConnect, {{":method", "CONNECT"}, {":authority", "example.com:443"}});
    EXPECT_TRUE(okResult.has_value()) << okResult.error().message;
    EXPECT_EQ(plainConnect.authorityText(), "example.com:443");

    Http3HeaderValidator withScheme(Http3MessageKind::Request);
    expectRejected(feedRequest(withScheme, {{":method", "CONNECT"}, {":scheme", "https"}, {":authority", "example.com"}}), Http3HeaderErrorKind::ProhibitedPseudoForMethod,
                   "经典 CONNECT 带了 :scheme");

    Http3HeaderValidator withoutAuthority(Http3MessageKind::Request);
    expectRejected(feedRequest(withoutAuthority, {{":method", "CONNECT"}}), Http3HeaderErrorKind::MissingPseudoHeader, "CONNECT 缺 :authority");
}

TEST(Http3HeaderValidation, ExtendedConnectNeedsPermissionAndKeepsSchemeAndPath)
{
    // RFC 9220 的扩展 CONNECT：开了权限才认 :protocol，且 :scheme/:path 照普通请求要求（隧道之上还要按路径路由）
    Http3HeaderValidator permitted(Http3MessageKind::Request, true);
    const auto okResult = feedRequest(permitted, {{":method", "CONNECT"}, {":scheme", "https"}, {":authority", "example.com"}, {":path", "/ws"}, {":protocol", "websocket"}});
    EXPECT_TRUE(okResult.has_value()) << okResult.error().message;
    EXPECT_EQ(permitted.protocolText(), "websocket");

    Http3HeaderValidator missingPath(Http3MessageKind::Request, true);
    expectRejected(feedRequest(missingPath, {{":method", "CONNECT"}, {":scheme", "https"}, {":authority", "example.com"}, {":protocol", "websocket"}}),
                   Http3HeaderErrorKind::MissingPseudoHeader, "扩展 CONNECT 缺 :path");

    Http3HeaderValidator protocolDuplicated(Http3MessageKind::Request, true);
    expectRejected(
            feedRequest(protocolDuplicated,
                        {{":method", "CONNECT"}, {":scheme", "https"}, {":authority", "example.com"}, {":path", "/ws"}, {":protocol", "websocket"}, {":protocol", "subprotocol"}}),
            Http3HeaderErrorKind::DuplicatePseudoHeader, "两个 :protocol");
}

TEST(Http3HeaderValidation, PathMustBePathAbsoluteOrAsterisk)
{
    for (const std::string_view invalidPath: {"", "json", "https://example.com/json", "/json#fragment", "/json space"})
    {
        Http3HeaderValidator validator(Http3MessageKind::Request);
        const auto           result = feedRequest(validator, requestWithFieldReplaced(3, ":path", invalidPath));
        expectRejected(result, Http3HeaderErrorKind::EmptyPath, invalidPath.empty() ? "空 :path" : invalidPath);
    }

    Http3HeaderValidator root(Http3MessageKind::Request);
    EXPECT_TRUE(feedRequest(root, requestWithFieldReplaced(3, ":path", "/")).has_value()) << "只带斜杠的路径是合法的";

    Http3HeaderValidator optionsStar(Http3MessageKind::Request);
    const auto           starResult = feedRequest(optionsStar, {{":method", "OPTIONS"}, {":scheme", "https"}, {":authority", "example.com"}, {":path", "*"}});
    EXPECT_TRUE(starResult.has_value()) << starResult.error().message << "；OPTIONS 的星号形式合法（RFC 9110 §7.1）";
}

TEST(Http3HeaderValidation, AuthorityCannotCarryUserInfoOrPath)
{
    for (const std::string_view invalidAuthority: {"user@example.com", "example.com/path", "example.com?x", "example.com#f", "exa mple"})
    {
        Http3HeaderValidator validator(Http3MessageKind::Request);
        const auto           result = feedRequest(validator, requestWithFieldReplaced(2, ":authority", invalidAuthority));
        expectRejected(result, Http3HeaderErrorKind::EmptyAuthority, invalidAuthority);
    }

    Http3HeaderValidator empty(Http3MessageKind::Request);
    expectRejected(feedRequest(empty, requestWithFieldReplaced(2, ":authority", "")), Http3HeaderErrorKind::EmptyAuthority, "空 :authority");

    Http3HeaderValidator ipv6(Http3MessageKind::Request);
    EXPECT_TRUE(feedRequest(ipv6, requestWithFieldReplaced(2, ":authority", "[::1]:8443")).has_value()) << "带方括号的 IPv6 权威应放行";
}

TEST(Http3HeaderValidation, SchemeDecidesWhetherAuthorityIsRequired)
{
    // §4.3.1：http/https 必须给权威；没有权威组件的方案则不得给
    Http3HeaderValidator httpWithoutAuthority(Http3MessageKind::Request);
    expectRejected(feedRequest(httpWithoutAuthority, {{":method", "GET"}, {":scheme", "http"}, {":path", "/"}}), Http3HeaderErrorKind::MissingPseudoHeader, "http 方案缺权威");

    Http3HeaderValidator hostOnly(Http3MessageKind::Request);
    const auto           hostResult = feedRequest(hostOnly, {{":method", "GET"}, {":scheme", "https"}, {":path", "/"}, {"host", "example.com"}});
    EXPECT_TRUE(hostResult.has_value()) << hostResult.error().message << "；只有 host 也满足要求";
    EXPECT_TRUE(hostOnly.hasHostHeader());
    EXPECT_TRUE(hostOnly.authorityText().empty()) << "没有 :authority 时不该凭空造一个";

    Http3HeaderValidator nonHttpWithAuthority(Http3MessageKind::Request);
    expectRejected(feedRequest(nonHttpWithAuthority, {{":method", "GET"}, {":scheme", "coap"}, {":authority", "example.com"}, {":path", "/t"}}),
                   Http3HeaderErrorKind::AuthorityConflict, "非 http/https 方案带权威");
}

TEST(Http3HeaderValidation, ConflictingAuthorityAndHostIsRejected)
{
    // :authority 与 host 同时存在时取值必须一致；不一致是请求走私的入口
    Http3HeaderValidator conflict(Http3MessageKind::Request);
    expectRejected(feedRequest(conflict, requestWithExtraField("host", "other.example")), Http3HeaderErrorKind::AuthorityConflict, ":authority 与 host 不一致");

    Http3HeaderValidator agreement(Http3MessageKind::Request);
    EXPECT_TRUE(feedRequest(agreement, requestWithExtraField("host", "example.com")).has_value()) << "两份取值一致时应当接受";
}

TEST(Http3HeaderValidation, RepeatedIdenticalHostFieldIsAcceptedButConflictingIsNot)
{
    // 头段里已经有 :authority，第一条同值 host 走「两项都在且一致」的分支被收下；
    // 这里钉的是 host 重复分支本身：同值放行、异值拒绝
    Http3HeaderValidator repeated(Http3MessageKind::Request);
    ASSERT_TRUE(feedRequest(repeated, requestWithExtraField("host", "example.com")).has_value());
    EXPECT_TRUE(repeated.onHeaderField("host", "example.com").has_value()) << "取值完全相同的第三份 host 不应被当成冲突";

    Http3HeaderValidator conflicting(Http3MessageKind::Request);
    ASSERT_TRUE(feedRequest(conflicting, requestWithExtraField("host", "example.com")).has_value());
    expectRejected(conflicting.onHeaderField("host", "evil.example"), Http3HeaderErrorKind::AuthorityConflict, "取值不同的第二份 host");
}

TEST(Http3HeaderValidation, EmptyHostValueIsRejected)
{
    Http3HeaderValidator validator(Http3MessageKind::Request);
    expectRejected(feedRequest(validator, {{":method", "GET"}, {":scheme", "https"}, {":path", "/"}, {"host", ""}}), Http3HeaderErrorKind::EmptyAuthority, "取值为空的 host");
}

TEST(Http3HeaderValidation, ResponseHeadRequiresASingleValidStatus)
{
    Http3HeaderValidator valid(Http3MessageKind::Response);
    ASSERT_TRUE(valid.beginHeaderBlock(false).has_value());
    ASSERT_TRUE(valid.onHeaderField(":status", "200").has_value());
    ASSERT_TRUE(valid.onHeaderField("content-type", "application/json").has_value());
    ASSERT_TRUE(valid.endHeaderBlock().has_value());
    ASSERT_TRUE(valid.statusCode().has_value());
    EXPECT_EQ(*valid.statusCode(), 200);

    Http3HeaderValidator missing(Http3MessageKind::Response);
    ASSERT_TRUE(missing.beginHeaderBlock(false).has_value());
    expectRejected(missing.endHeaderBlock(), Http3HeaderErrorKind::MissingPseudoHeader, "响应缺 :status");

    for (const std::string_view invalidStatus: {"20", "2000", "abc", "2xx", "0200", "99"})
    {
        Http3HeaderValidator badValue(Http3MessageKind::Response);
        ASSERT_TRUE(badValue.beginHeaderBlock(false).has_value());
        expectRejected(badValue.onHeaderField(":status", invalidStatus), Http3HeaderErrorKind::InvalidStatusValue, invalidStatus);
    }

    // 0xx 与「首位不是 1-9」的三位数都不合法：三位十进制是 §4.3.2 的硬要求
    Http3HeaderValidator leadingZero(Http3MessageKind::Response);
    ASSERT_TRUE(leadingZero.beginHeaderBlock(false).has_value());
    expectRejected(leadingZero.onHeaderField(":status", "000"), Http3HeaderErrorKind::InvalidStatusValue, "000 不是合法状态码");

    Http3HeaderValidator duplicated(Http3MessageKind::Response);
    ASSERT_TRUE(duplicated.beginHeaderBlock(false).has_value());
    ASSERT_TRUE(duplicated.onHeaderField(":status", "204").has_value());
    expectRejected(duplicated.onHeaderField(":status", "204"), Http3HeaderErrorKind::DuplicatePseudoHeader, "第二个 :status");

    Http3HeaderValidator methodInResponse(Http3MessageKind::Response);
    ASSERT_TRUE(methodInResponse.beginHeaderBlock(false).has_value());
    expectRejected(methodInResponse.onHeaderField(":method", "GET"), Http3HeaderErrorKind::UndefinedPseudoHeader, "响应里出现请求伪头");
}

TEST(Http3HeaderValidation, RejectedSectionsStillLeaveNoPartialPseudoHeaders)
{
    // 钉住「拒绝之后不留脏状态」：第一条字段就非法时，伪头不该被归位一半
    Http3HeaderValidator validator(Http3MessageKind::Request);
    ASSERT_TRUE(validator.beginHeaderBlock(false).has_value());
    expectRejected(validator.onHeaderField(":method", std::string_view("GET\nPOST", 8)), Http3HeaderErrorKind::IllegalFieldValueCharacter, "含 LF 的 :method");
    EXPECT_TRUE(validator.methodText().empty()) << "取值非法的伪头不得写进归位结果";
    EXPECT_FALSE(validator.endHeaderBlock().has_value()) << "被拒的伪头没归位，收尾时必然缺必填项";
}
