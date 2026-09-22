// 配置取值热路径的分配画像：getString 取一份长字符串要过几次堆
//
// 口径与 tests/Base/Log/TestLogHotPathAllocations.cpp 一致（共用 AllocationProbe）：
//   · 参照形状是「把返回串本身拷一份」——任何实现都省不掉这一块；
//   · 被测形状走 getString 全通道，命中一个 40 字节的字符串键；
//   · 相减就是「查表 + 类型判定 + 交回调用方」这段付的分配。
// 分配判据只在 Release 下钉死：Debug 的 STL 迭代器调试代理会给每次查找多挂一块代理，
// 读数被实现细节放大一个量级，钉住它等于把判据交给编译器实现。

#include "Base/Config/ConfigManager.h"
#include "Base/Config/ConfigValue.h"

#include "AllocationProbe.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Base
{
    namespace
    {
        using AsynGyanis::TestSupport::AllocationProfile;
        using AsynGyanis::TestSupport::kMeasurementIterations;
        using AsynGyanis::TestSupport::measurePerOperation;
        using AsynGyanis::TestSupport::resetAllocationHistogram;

        /// 被测取值：40 字节，长过小串内联，返回串那块堆一定会被记到
        constexpr std::string_view kValueText = "a-config-string-value-above-small-string";

        /// 默认值同样取到小串之外：短到内联的默认值会让「进入时先造一份默认值串」这一步不碰堆，
        /// 读数就看不出那条通道白花了几次
        constexpr std::string_view kFallbackText = "a-fallback-string-value-above-small-st";
    } // namespace

    /**
     * @brief getString 取一份字符串的分配读数
     * @details 判据是「不超过返回串本身那一块」。取用通道原先经 get<std::string>(key, 默认值)：
     *          默认值按值再造一份、getOptional 把整份 ConfigValue 深拷进 optional、
     *          再从副本里拷一次字符串，一次取值过三次堆而调用方只要一份文本。
     */
    TEST(ConfigHotPathAllocations, GetStringReading)
    {
        const std::string fallback{ kFallbackText };
        auto             &manager = ConfigManager::instance();
        manager.clear();
        ASSERT_TRUE(manager.setValue("app.text", ConfigValue(std::string(kValueText))));

        // 参照形状：只把字符串本身拷一份出来，这是任何实现都省不掉的一块
        const ConfigValue stored = ConfigValue(std::string(kValueText));
        const auto         copyReferenceOnce = [&stored]
        {
            const std::string text = stored.get_ref<const std::string &>();
            return static_cast<std::uint64_t>(text.size());
        };
        resetAllocationHistogram();
        const AllocationProfile referenceProfile = measurePerOperation(copyReferenceOnce);

        const auto getStringOnce = [&manager, &fallback]
        {
            return static_cast<std::uint64_t>(manager.getString("app.text", fallback).size());
        };
        getStringOnce();                   // 先把快照里的形状热出来，测的是稳态
        resetAllocationHistogram();
        const AllocationProfile profile = measurePerOperation(getStringOnce);

        // 两条读数都得是「一千次都取到了那 40 个字符」，否则可能测的是没命中的冷路径
        EXPECT_EQ(referenceProfile.resultSum, kMeasurementIterations * kValueText.size());
        EXPECT_EQ(profile.resultSum, kMeasurementIterations * kValueText.size());
#ifdef NDEBUG
        // 上限给 1/64 的增长余量：快照原子量与哈希表在首轮可能各要长一次容量，不钉死。
        // 未修时这一千次多付两千次以上，远超本上限
        EXPECT_LE(profile.totalAllocations, referenceProfile.totalAllocations + kMeasurementIterations / 64U)
                << "getString 还在多层拷贝之间过堆：取用通道没有直接按视图查表";
#endif
        std::printf("get-string per-op=%llu total=%llu bytes=%llu (ref total=%llu)\n",
                    static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.totalAllocations),
                    static_cast<unsigned long long>(profile.totalBytes),
                    static_cast<unsigned long long>(referenceProfile.totalAllocations));

        manager.clear();
    }
} // namespace AsynGyanis::Base
