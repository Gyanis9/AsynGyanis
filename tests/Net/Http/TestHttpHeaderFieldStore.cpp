// TestHttpHeaderFieldStore.cpp —— 头部存储的读取侧语义：单值查询（get）、首条取值（firstValue）、
//   列表 token 判定（containsListToken）与整表视图（singleValueView）。四条读路径必须给出同一套
//   口径，而 get 现在直接走权威记录、不再顺手把视图建出来。
//   这里钉的都是两条实现容易分叉的地方：
//   一. 同名多条的 ", " 合并（RFC 7230 §3.2.2），含「值为空串」这一格——判「有没有首条」
//       只能用 optional 是否 engaged，用 empty() 判会把第二条的分隔符吞掉；
//   二. 可重复头部（Set-Cookie）取首条且不合并；
//   三. 单值查询不得让视图变干净或变脏（先查后取整表、先取整表后查，结果都要一致）；
//   四. 删名之后单值查询与视图同步失去该条目（不留「查得到、序列化里没有」的鬼条目）；
//   五. 四条读路径都按「大小写不敏感」认键（不要求调用方先归一化），且判定只看整 token、
//       逗号列表的畸形写法不能让循环不推进（取值由对端控制）。

#include "Net/Http/HttpHeaderFieldStore.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 建一个装了若干条头部的存储
         * @param pairs 名值对，按加入顺序
         * @return HttpHeaderFieldStore 装好的存储
         */
        HttpHeaderFieldStore makeStore(const std::vector<std::pair<std::string, std::string>> &pairs)
        {
            HttpHeaderFieldStore store;
            for (const auto &[name, value]: pairs)
            {
                store.append(name, value);
            }
            return store;
        }
    } // namespace

    TEST(HttpHeaderFieldStore, SingleValueMergesRepeatsOfAnOrdinaryHeader)
    {
        const HttpHeaderFieldStore store = makeStore({{"Accept-Encoding", "gzip"},
                                                     {"host", "example.com"},
                                                     {"accept-encoding", "br"},
                                                     {"Accept-Encoding", "zstd"}});

        // 大小写不同的键走同一套归一化；三条按加入顺序合并
        EXPECT_EQ(store.get("accept-encoding").value_or("<缺失>"), "gzip, br, zstd");
        EXPECT_EQ(store.get("ACCEPT-ENCODING").value_or("<缺失>"), "gzip, br, zstd");
        EXPECT_EQ(store.singleValueView().at("accept-encoding"), "gzip, br, zstd")
                << "单值查询与整表视图口径分叉了";
    }

    TEST(HttpHeaderFieldStore, SingleValueMergesEvenWhenTheFirstValueIsEmpty)
    {
        // 首条值为空串：判「有没有首条」只能看 optional 是否已装值，
        // 拿 empty() 当判据会把第二条的分隔符一起吞掉
        const HttpHeaderFieldStore store = makeStore({{"x-empty", ""}, {"x-empty", ""}});

        EXPECT_EQ(store.get("x-empty").value_or("<缺失>"), ", ");
        EXPECT_EQ(store.singleValueView().at("x-empty"), ", ");
    }

    TEST(HttpHeaderFieldStore, SingleValueKeepsAnEmptyValueAsAHit)
    {
        const HttpHeaderFieldStore store = makeStore({{"x-blank", ""}});

        const std::optional<std::string> blank = store.get("x-blank");
        ASSERT_TRUE(blank.has_value()) << "空值头部是命中，不是缺席";
        EXPECT_TRUE(blank->empty());
        EXPECT_FALSE(store.get("x-absent").has_value()) << "缺席必须用 optional 空值表达，不能回空串";
    }

    TEST(HttpHeaderFieldStore, SingleValueTakesFirstOccurrenceOfRepeatableHeader)
    {
        const HttpHeaderFieldStore store = makeStore({{"set-cookie", "sid=1; Path=/"},
                                                     {"set-cookie", "theme=dark, admin"},
                                                     {"Set-Cookie", "locale=zh-CN"}});

        // cookie 值本身可以含逗号，合并后就再也切不回去，因此视图与 get 都只留首条
        EXPECT_EQ(store.get("set-cookie").value_or("<缺失>"), "sid=1; Path=/");
        EXPECT_EQ(store.singleValueView().at("set-cookie"), "sid=1; Path=/");
        EXPECT_EQ(store.values("set-cookie").size(), 3U) << "权威记录要逐条留档，供序列化与 headerValues 使用";
    }

    TEST(HttpHeaderFieldStore, SingleValueQueryLeavesTheLazyViewStateIntact)
    {
        HttpHeaderFieldStore store = makeStore({{"host", "example.com"}, {"accept", "*/*"}});

        // 先做单值查询：视图既不该被提前建出来，也不该被标脏
        EXPECT_EQ(store.get("host").value_or("<缺失>"), "example.com");
        EXPECT_EQ(store.singleValueView().size(), 2U);
        EXPECT_EQ(store.singleValueView().at("accept"), "*/*");

        // 建完视图之后再查单值，结果仍与视图一致
        EXPECT_EQ(store.get("accept").value_or("<缺失>"), "*/*");
    }

    TEST(HttpHeaderFieldStore, RemovedNameDisappearsFromBothReadPaths)
    {
        HttpHeaderFieldStore store = makeStore({{"x-gone", "1"}, {"x-gone", "2"}, {"host", "example.com"}});

        // 先把视图建出来（让它带上旧条目），再删名：两条读路径都得反映删除
        ASSERT_EQ(store.singleValueView().count("x-gone"), 1U);
        store.removeAll("x-gone");

        EXPECT_FALSE(store.get("x-gone").has_value()) << "删掉的头部仍被查到，就是「查得到、序列化里没有」的鬼条目";
        EXPECT_EQ(store.singleValueView().count("x-gone"), 0U);
        EXPECT_TRUE(store.values("x-gone").empty());
        EXPECT_EQ(store.singleValueView().at("host"), "example.com") << "删一个名字不得牵连其它条目";
    }

    TEST(HttpHeaderFieldStore, OverwriteKeepsPositionAndSingleValueAgrees)
    {
        HttpHeaderFieldStore store = makeStore({{"host", "old.example.com"}, {"accept", "*/*"}});
        store.overwriteOrAppend("host", "new.example.com");

        EXPECT_EQ(store.get("host").value_or("<缺失>"), "new.example.com");
        EXPECT_EQ(store.singleValueView().at("host"), "new.example.com");
        // 覆盖不追加条目：序列化顺序仍停在首次设置处
        ASSERT_EQ(store.fields().size(), 2U);
        EXPECT_EQ(store.fields()[0].name, "host");
    }

    TEST(HttpHeaderFieldStore, FirstValueTakesTheEarliestFieldAndNeverMerges)
    {
        const HttpHeaderFieldStore store = makeStore({{"x-request-id", "trace-a"},
                                                     {"host", "example.com"},
                                                     {"x-request-id", "trace-b"}});

        // 首条原样交出：同名多条时 get() 会按 ", " 合并，那条口径不适合链路 id
        EXPECT_EQ(store.firstValue("x-request-id").value_or("<缺失>"), "trace-a");
        EXPECT_EQ(store.firstValue("x-request-id"), store.values("x-request-id").front())
                << "firstValue 必须与 values() 的首元素同值，否则两条读路径会分叉";
        EXPECT_FALSE(store.firstValue("x-absent").has_value());
    }

    TEST(HttpHeaderFieldStore, ReadPathsIgnoreTheNameCaseWithoutCanonicalizingIt)
    {
        const HttpHeaderFieldStore store = makeStore({{"x-request-id", "trace-a"}, {"connection", "close"}});

        // 读路径不要求调用方先归一化：入库名已是小写，比较时两侧就地折 ASCII 大小写，
        // 于是长头部名（超过短字符串缓冲）不必为一次查询现造一个归一化副本
        EXPECT_EQ(store.firstValue("X-Request-Id").value_or("<缺失>"), "trace-a");
        EXPECT_TRUE(store.containsListToken("Connection", "close"));
        EXPECT_TRUE(store.get("X-Request-Id").has_value());
        EXPECT_EQ(store.values("X-Request-Id").size(), 1U);
        // 显式归一化入口仍然可用，包装层与序列化按它取小写形态
        EXPECT_TRUE(store.firstValue(HttpHeaderFieldStore::toCanonicalHeaderName("X-Request-Id")).has_value());
    }

    TEST(HttpHeaderFieldStore, MixedCaseCookieNameStillTakesTheRepeatablePath)
    {
        // 可重复头部的判定名单登记的是小写；读侧不再先归一化查询名，因此名单比对也必须
        // 大小写不敏感——否则 "Set-Cookie" 会被当成普通头部按 ", " 合并，把两条独立 cookie 粘死
        const HttpHeaderFieldStore store = makeStore({{"set-cookie", "sid=1; Path=/"},
                                                     {"Set-Cookie", "theme=dark, admin"}});

        EXPECT_EQ(store.get("Set-Cookie").value_or("<缺失>"), "sid=1; Path=/");
        EXPECT_EQ(store.get("SET-COOKIE").value_or("<缺失>"), "sid=1; Path=/");
        EXPECT_EQ(store.values("Set-Cookie").size(), 2U) << "权威记录仍逐条留档";
    }

    TEST(HttpHeaderFieldStore, ContainsListTokenSplitsOnCommasAndIgnoresOwsAndCase)
    {
        const HttpHeaderFieldStore store = makeStore({{"connection", "Keep-Alive, Upgrade"},
                                                     {"x-flag", ""},
                                                     {"x-flag", "  trailing , 中段  "}});

        EXPECT_TRUE(store.containsListToken("connection", "keep-alive"));
        EXPECT_TRUE(store.containsListToken("connection", "upgrade"));
        // 段首尾的 OWS 会被裁掉，段内的大小写不敏感
        EXPECT_TRUE(store.containsListToken("x-flag", "trailing"));
        EXPECT_TRUE(store.containsListToken("x-flag", "中段"));
        // 值为空串的那条不构成任何 token，但不该影响同名第二条的判定
        EXPECT_TRUE(store.containsListToken("x-flag", "TrAiLiNg"));
    }

    TEST(HttpHeaderFieldStore, ContainsListTokenMatchesWholeTokensOnly)
    {
        const HttpHeaderFieldStore store = makeStore({{"connection", "keep-aliveish, xkeep-alive, close-ish"}});

        // 拒绝面：子串不算命中。把 keep-alive 当成 "keep-aliveish" 的一部分，
        // 就会让一条本不该保活的连接被保持，而这类误判在日志里完全看不出来
        EXPECT_FALSE(store.containsListToken("connection", "keep-alive"));
        EXPECT_FALSE(store.containsListToken("connection", "close"));
        EXPECT_TRUE(store.containsListToken("connection", "close-ish"));
        EXPECT_FALSE(store.containsListToken("x-absent", "keep-alive"));
    }

    TEST(HttpHeaderFieldStore, ContainsListTokenTerminatesOnMalformedCommaLists)
    {
        const HttpHeaderFieldStore emptyValue;
        EXPECT_FALSE(emptyValue.containsListToken("connection", "close"));

        const HttpHeaderFieldStore dangling = makeStore({{"connection", "close,"}, {"x-edge", ","}});
        // 尾逗号与「只有一个逗号」都不能让循环不推进：这条路径对端可控，挂住就是拒绝服务
        EXPECT_TRUE(dangling.containsListToken("connection", "close"));
        EXPECT_FALSE(dangling.containsListToken("x-edge", "close"));
    }

    TEST(HttpHeaderFieldStore, WritePathsMatchTheStoredNameIgnoringCase)
    {
        HttpHeaderFieldStore store = makeStore({{"content-type", "text/plain"}});

        // 覆盖式写入即便用原大小写也必须命中既有记录：命不中就多出一条同名头部，
        // 序列化时两条一起上线，收端按哪条读都是错位
        store.overwriteOrAppend("CONTENT-TYPE", "application/json");
        ASSERT_EQ(store.fields().size(), 1U) << "大小写不同的同名写入不得新增条目";
        EXPECT_EQ(store.fields()[0].name, "content-type") << "入库形态必须是小写，序列化按它上线";
        EXPECT_EQ(store.get("content-type").value_or("<缺失>"), "application/json");

        // 缺席时新建条目，名同样折成小写入库
        store.overwriteOrAppend("X-Trace-Id", "abc");
        ASSERT_EQ(store.fields().size(), 2U);
        EXPECT_EQ(store.fields()[1].name, "x-trace-id");

        store.removeAll("X-TRACE-ID");
        EXPECT_EQ(store.fields().size(), 1U) << "删名也要大小写不敏感，否则中间件清不掉处理器写下的头部";
    }

} // namespace AsynGyanis::Net
