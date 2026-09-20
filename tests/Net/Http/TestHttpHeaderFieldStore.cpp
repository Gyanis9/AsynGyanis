// TestHttpHeaderFieldStore.cpp —— 头部存储的读取侧语义：单值查询（get）与整表视图
//   （singleValueView）必须给出同一套口径，而 get 现在直接走权威记录、不再顺手把视图建出来。
//   这里钉的都是两条实现容易分叉的地方：
//   一. 同名多条的 ", " 合并（RFC 7230 §3.2.2），含「值为空串」这一格——判「有没有首条」
//       只能用 optional 是否 engaged，用 empty() 判会把第二条的分隔符吞掉；
//   二. 可重复头部（Set-Cookie）取首条且不合并；
//   三. 单值查询不得让视图变干净或变脏（先查后取整表、先取整表后查，结果都要一致）；
//   四. 删名之后单值查询与视图同步失去该条目（不留「查得到、序列化里没有」的鬼条目）。

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

} // namespace AsynGyanis::Net
