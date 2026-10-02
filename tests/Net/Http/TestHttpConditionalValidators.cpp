// 条件请求验证器比较的直测：弱比较与强比较的差别是规范规定的，必须分别钉住
//
// 这里只测判据本身（纯函数、不起服务器）；端到端的 412/304 判定在
// tests/Net/Http/TestHttpServer.cpp 的 RejectsStaleIfMatchWith412 里。

#include "Net/Http/HttpConditionalValidators.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 给标签套上 DQUOTE（ETag 的语法形式），让用例里的字面量不必到处写转义
         * @param core 引号内的标签主体
         * @return std::string 形如 `"abc"` 的标签
         */
        std::string quoted(const std::string_view core)
        {
            return '"' + std::string(core) + '"';
        }
    } // namespace

    /**
     * @brief 钉住：If-None-Match 走弱比较——剥 W/、* 在列表里也命中任意资源
     * @details 期望值全部手写，不调用被测实现算期望
     */
    TEST(HttpConditionalValidators, WeakComparisonIgnoresTheWeaknessFlag)
    {
        const std::string abc = quoted("abc");

        EXPECT_TRUE(weakEntityTagListMatches(abc, abc));
        EXPECT_TRUE(weakEntityTagListMatches("W/" + abc, abc)) << "弱标记不参与比较，这是 §8.8.3.2 的要求";
        EXPECT_TRUE(weakEntityTagListMatches("*", abc)) << "整值为 * 命中任意资源";
        EXPECT_TRUE(weakEntityTagListMatches(R"("x", *, "y")", abc)) << "* 作为列表一项同样命中任意资源";
        EXPECT_TRUE(weakEntityTagListMatches(R"("x","y")", quoted("y"))) << "逗号后没有空格也要切得开";
        EXPECT_FALSE(weakEntityTagListMatches(R"("x", "y")", quoted("z")));
        EXPECT_FALSE(weakEntityTagListMatches("", abc)) << "空值不参与判定，也不能把空串当成通配";
    }

    /**
     * @brief 钉住：If-Match 走强比较——W/ 永不命中，而 * 只在整值时代表「资源还在」
     * @details 这两点与上面弱比较的差别正是 §13.1.2 与 §13.2.1 分别规定的；复用同一份比较
     *          就等于悄悄放宽了写前提
     */
    TEST(HttpConditionalValidators, StrongComparisonRejectsWeakValidatorsAndListedWildcard)
    {
        const std::string abc = quoted("abc");

        EXPECT_TRUE(strongEntityTagListMatches(abc, abc, true));
        EXPECT_FALSE(strongEntityTagListMatches("W/" + abc, abc, true)) << "弱标签永不强匹配";
        EXPECT_TRUE(strongEntityTagListMatches("*", abc, true)) << "整值 * 表达「只要它还在」";
        EXPECT_FALSE(strongEntityTagListMatches("*", abc, false)) << "资源不存在时 * 不成立";
        EXPECT_FALSE(strongEntityTagListMatches(R"("x", *, "y")", quoted("z"), true)) << "* 作为列表一项不代表任意资源";
        EXPECT_TRUE(strongEntityTagListMatches(R"("x", "z")", quoted("z"), true)) << "列表里逐项强比较";
        EXPECT_TRUE(strongEntityTagListMatches("  " + quoted("z") + "  ", quoted("z"), true)) << "首尾 SP/HTAB 要裁掉";
        EXPECT_FALSE(strongEntityTagListMatches("", quoted("z"), true)) << "空列表按不命中处理，不解释成任意";
    }

    /**
     * @brief 钉住：小写 w/ 不是弱标记（§8.8.3 的语法固定为大写）
     */
    TEST(HttpConditionalValidators, LowercaseWeakPrefixIsNotAMarker)
    {
        EXPECT_EQ(stripWeakValidatorPrefix(R"(w/"abc")"), R"(w/"abc")");
        EXPECT_EQ(stripWeakValidatorPrefix(R"(W/"abc")"), R"("abc")");
        EXPECT_TRUE(isWeakValidator(R"(W/"abc")"));
        EXPECT_FALSE(isWeakValidator(R"(w/"abc")"));
    }
} // namespace AsynGyanis::Net
