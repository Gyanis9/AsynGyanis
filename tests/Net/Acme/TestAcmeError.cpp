// acmeErrorKindName 的全枚举映射直测：每一档都要有自己的短名，且不重名
//
// 这张表住在日志与 status() 的读数里，此前没有任何用例读过它：把某一档错折成
// 「unknown」，面板与日志上那条失败就与真正的未知种类同形，而编译与全部既有用例都照旧绿。

#include "Net/Acme/AcmeError.h"

#include <gtest/gtest.h>

#include <array>
#include <string_view>
#include <unordered_set>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 全部失败种类，按声明顺序列出：新增一档而这里没跟上，第一条断言就会失败
        constexpr std::array<AcmeErrorKind, 10> kAllErrorKinds{
                AcmeErrorKind::InvalidConfiguration, AcmeErrorKind::KeyMaterial,         AcmeErrorKind::Transport,
                AcmeErrorKind::UnexpectedResponse,   AcmeErrorKind::RejectedByAuthority, AcmeErrorKind::RateLimited,
                AcmeErrorKind::ChallengeNotAnswered, AcmeErrorKind::DnsRecordRejected,   AcmeErrorKind::FileSystem,
                AcmeErrorKind::ReloadRejected,
        };
    } // namespace

    /**
     * @brief 钉住：每一档都有可区分的 ASCII 短名，且没有任何一档折成 unknown
     * @details 期望值写字面量而不是调实现算——这张表的真源是种类本身，实现自己说自己在说什么
     *          就等于没钉（规范/文案一改就两边一起变，用例照绿）
     */
    TEST(AcmeError, EveryKindHasItsOwnShortName)
    {
        const std::array<std::string_view, kAllErrorKinds.size()> kExpectedNames{
                "InvalidConfiguration", "KeyMaterial",          "Transport",         "UnexpectedResponse", "RejectedByAuthority",
                "RateLimited",          "ChallengeNotAnswered", "DnsRecordRejected", "FileSystem",         "ReloadRejected",
        };

        for (std::size_t index = 0; index < kAllErrorKinds.size(); ++index)
        {
            const std::string_view name = acmeErrorKindName(kAllErrorKinds[index]);
            EXPECT_EQ(name, kExpectedNames[index]) << "第 " << index << " 档的短名与登记表不符";
            EXPECT_NE(name, "unknown") << "有一档没被表接住：日志里它与真正的未知种类同形";
        }
    }

    /**
     * @brief 钉住：十个短名两两不同
     * @details 这张表的用途是让运维从一个词判断该做什么处置；两档共用一个名字就等于把两种处置
     *          并成一种，而并掉的那一种恰恰可能是要立刻处置的那个
     */
    TEST(AcmeError, ShortNamesArePairwiseDistinct)
    {
        std::unordered_set<std::string_view> seen;
        for (const AcmeErrorKind kind: kAllErrorKinds)
        {
            const auto [_, inserted] = seen.insert(acmeErrorKindName(kind));
            EXPECT_TRUE(inserted) << "短名重复：" << acmeErrorKindName(kind);
        }
        EXPECT_EQ(seen.size(), kAllErrorKinds.size());
    }
} // namespace AsynGyanis::Net
