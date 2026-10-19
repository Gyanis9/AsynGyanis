// 解码层「报文坏了」那一条出口每次付出多少次堆分配。这一族与成功路径不同：一条对端可控的坏包
// 就能走进去，而它今天在里面现拼一句中文（`std::format` 把取值、剩余字节数与 RFC 章节拼成一条串）。
// 正常流量下这条几乎不走，压力全在「垃圾包成千成湿地来」那一侧——所以按一千次坏包摊平来量。
//
// 量法与 Http 那一族同一套（见 tests/Net/Http/TestHotPathAllocations.cpp 的开头）：被测体里不放断言，
// 改成返回一个与「判成了坏包」成正比的标记；先跑一次本底自证探针还看得见分配。
//
// 读数（Linux 容器，GCC 13.3，ASan/UBSan 那一档）：
//   · 变长整数读断：每次 1 次 / 127 字节，就是那一句判词本身；
//   · 报文头读到版本号就断：每次 1 次 / 159 字节，同上；
//   · 帧层读到一条断掉的 STREAM 帧：每次 5 次 / 497 字节——游标把帧名格式化成一条临时串、明细拼一条、
//     外层再拼一句，串本体一条。
// 钉的是上界而不是精确值：判词是一句中文串，串长与是否落到堆随标准库的小串内联容量变。上界防的方向只有
// 一个——往后谁在失败路径上又加一次分配。把判词推到上报处（不在解码层拼）本仓不取：那会让每个消费点重拼
// 同一句，而这一条出口的代价与一次坏包的收包成本同量级。

#include "Net/Quic/Codec/QuicFrame.h"
#include "Net/Quic/Codec/QuicPacketHeader.h"
#include "Net/Quic/Codec/QuicVariableLengthInteger.h"

#include "AllocationProbe.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using AsynGyanis::TestSupport::AllocationProfile;
        using AsynGyanis::TestSupport::kMeasurementIterations;
        using AsynGyanis::TestSupport::measurePerOperation;

        /// 本底自证用的串长：超过小串内联容量，每次都要一块堆
        constexpr std::size_t kBaselineStringByteCount = 40U;

        /// 变长整数：首字节 0xC0 声明本数占 2 字节，而缓冲区只给到 1 字节
        constexpr std::uint8_t kTruncatedVarintFirstByte = 0xC0U;

        /// 长头报文的首字节（固定位为 1，Version 1 的 Initial），后面只给到版本号，连 DCID 长度那一字节都不到
        constexpr std::uint8_t kLongHeaderInitialFirstByte = 0xC0U;

        /// 只到版本号末尾的字节数：1 字节首字节 + 4 字节版本号
        constexpr std::size_t kHeaderTruncatedByteCount = 5U;

        /// STREAM 帧的类型字节，后面什么都不给——帧连流标识都读不出来
        constexpr std::uint8_t kStreamFrameType = 0x08U;

        /// 一句判词的上界：实测每次 1 次分配，留 1 次给标准库缓冲形状的差异
        constexpr std::uint64_t kSingleMessageAllocationCeiling = 2U;

        /// 帧层那一句判词的上界：帧名、明细、外层拼接与串本体，实测每次 5 次
        constexpr std::uint64_t kFrameMessageAllocationCeiling = 6U;
    } // namespace

    /**
     * @brief 本底自证：探针必须看得见一次明确的堆申请
     * @details 少了这一条，后面所有「读数 = 0」都可能只是钩子被顶掉了
     */
    TEST(QuicDecodeFailureAllocations, BaselineProvesTheProbeIsAlive)
    {
        ASYN_SKIP_IF_ALLOCATION_PROBE_IS_BLIND();

        const AllocationProfile profile = measurePerOperation(
                []
                {
                    const std::string text(kBaselineStringByteCount, 'x');
                    return static_cast<std::uint64_t>(text.size());
                });

        std::printf("probe-baseline 每次分配 %llu 次 / %llu 字节（一千次共 %llu 次）\n", static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.bytesPerOperation), static_cast<unsigned long long>(profile.totalAllocations));
        EXPECT_EQ(profile.resultSum, kMeasurementIterations * kBaselineStringByteCount);
        EXPECT_GT(profile.totalAllocations, 0U) << "探针看不见分配，这一族的读数全部不可信";
    }

    /**
     * @brief 量：变长整数读断之后，判词之外还付几次堆
     */
    TEST(QuicDecodeFailureAllocations, MeasuresTheTruncatedVariableLengthIntegerPath)
    {
        ASYN_SKIP_IF_ALLOCATION_PROBE_IS_BLIND();

        const std::vector<std::uint8_t> bytes{kTruncatedVarintFirstByte};

        const AllocationProfile profile = measurePerOperation(
                [&bytes]
                {
                    const auto outcome = decodeQuicVariableLengthInteger(std::span<const std::uint8_t>{bytes});
                    return static_cast<std::uint64_t>(!outcome.has_value() && outcome.error().kind == QuicDecodeErrorKind::Truncated);
                });

        std::printf("varint-truncated 每次分配 %llu 次 / %llu 字节（一千次共 %llu 次 / %llu 字节）\n", static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.bytesPerOperation), static_cast<unsigned long long>(profile.totalAllocations),
                    static_cast<unsigned long long>(profile.totalBytes));
        EXPECT_EQ(profile.resultSum, kMeasurementIterations) << "有几次没按「截断」判出来";
        EXPECT_LE(profile.totalAllocations, kMeasurementIterations * kSingleMessageAllocationCeiling) << "变长整数这一出口每包多付了一次堆：判词之外不该再有别的分配";
    }

    /**
     * @brief 量：数据报头解到一半断掉，每个垃圾包付几次堆
     * @details 这一条是「垃圾包成千成湿地来」那一侧的形状：每个不认识的包都要过一遍报文头解码
     */
    TEST(QuicDecodeFailureAllocations, MeasuresTheTruncatedPacketHeaderPath)
    {
        ASYN_SKIP_IF_ALLOCATION_PROBE_IS_BLIND();

        const std::vector<std::uint8_t> datagram(kHeaderTruncatedByteCount, kLongHeaderInitialFirstByte);

        const AllocationProfile profile = measurePerOperation(
                [&datagram]
                {
                    const auto outcome = decodeQuicPacketHeader(std::span<const std::uint8_t>{datagram}, std::size_t{8});
                    return static_cast<std::uint64_t>(!outcome.has_value());
                });

        std::printf("header-truncated 每次分配 %llu 次 / %llu 字节（一千次共 %llu 次 / %llu 字节）\n", static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.bytesPerOperation), static_cast<unsigned long long>(profile.totalAllocations),
                    static_cast<unsigned long long>(profile.totalBytes));
        EXPECT_EQ(profile.resultSum, kMeasurementIterations) << "有几次没被判成坏头";
        EXPECT_LE(profile.totalAllocations, kMeasurementIterations * kSingleMessageAllocationCeiling) << "报文头这一出口每包多付了一次堆：判词之外不该再有别的分配";
    }

    /**
     * @brief 量：帧层读到一条断掉的 STREAM 帧时付几次堆
     */
    TEST(QuicDecodeFailureAllocations, MeasuresTheMalformedFramePath)
    {
        ASYN_SKIP_IF_ALLOCATION_PROBE_IS_BLIND();

        const std::vector<std::uint8_t> payload{kStreamFrameType};

        const AllocationProfile profile = measurePerOperation(
                [&payload]
                {
                    std::vector<QuicFrame> frames;
                    const auto             outcome = decodeQuicFrames(std::span<const std::uint8_t>{payload}, frames);
                    return static_cast<std::uint64_t>(!outcome.has_value());
                });

        std::printf("frame-malformed 每次分配 %llu 次 / %llu 字节（一千次共 %llu 次 / %llu 字节）\n", static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.bytesPerOperation), static_cast<unsigned long long>(profile.totalAllocations),
                    static_cast<unsigned long long>(profile.totalBytes));
        EXPECT_EQ(profile.resultSum, kMeasurementIterations) << "有几次没被判成坏帧";
        EXPECT_LE(profile.totalAllocations, kMeasurementIterations * kFrameMessageAllocationCeiling) << "帧层这一出口每次多付了一次堆：帧名、明细与外层拼接已是全部预算";
    }
} // namespace AsynGyanis::Net
