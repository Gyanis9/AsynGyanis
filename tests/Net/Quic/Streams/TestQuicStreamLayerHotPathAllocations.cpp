// TestQuicStreamLayerHotPathAllocations.cpp —— QUIC 流层的每连接分配画像
//
// 流层是「每条连接一份」的对象，它的固定成本直接乘在连接数上，所以单独量两条形状：
//   1) 光把这个对象建出来再析构——量的是空载容器的预订开销，一个字节都没收发；
//   2) 建出来之后收一条带正文的请求、交付完、把额度还回去——量稳态每连接的实用成本。
// 两条都只打印读数不设阈值：它们的存在理由是把改动前后的对照数字钉在能跑出来的地方。

#include "Net/Quic/Codec/QuicTransportParameters.h"
#include "Net/Quic/Streams/QuicStreamLayer.h"

#include "AllocationProbe.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using AsynGyanis::TestSupport::AllocationProfile;
        using AsynGyanis::TestSupport::kMeasurementIterations;
        using AsynGyanis::TestSupport::measurePerOperation;

        /**
         * @brief 只填流层真正会读的那几项传输参数
         * @return QuicTransportParameters 连接级 4096、各类流级 1024、双向 4 条、单向 2 条
         */
        QuicTransportParameters makeStreamLayerParameters()
        {
            QuicTransportParameters parameters;
            parameters.initialMaximumData                           = 4096;
            parameters.initialMaximumStreamDataBidirectionalLocal   = 1024;
            parameters.initialMaximumStreamDataBidirectionalRemote  = 1024;
            parameters.initialMaximumStreamDataUnidirectional       = 1024;
            parameters.initialMaximumBidirectionalStreams           = 4;
            parameters.initialMaximumUnidirectionalStreams          = 2;
            return parameters;
        }
    } // namespace

    /**
     * @brief 光建一条连接的流状态、一个字节都没收发，付出多少次分配
     */
    TEST(QuicStreamLayerAllocations, ConstructionAllocations)
    {
        const auto constructOnce = []() -> std::size_t
        {
            const QuicStreamLayer layer(makeStreamLayerParameters());
            return layer.trackedStreamCount();
        };
        static_cast<void>(constructOnce());

        const AllocationProfile profile = measurePerOperation(constructOnce);
        std::printf("quic 流层每条连接的构造成本 %llu 次分配 / %llu 字节\n",
                    static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.bytesPerOperation));
    }

    /**
     * @brief 建一条连接并收完交付完一条 16 字节正文的请求，付出多少次分配
     * @details 交付与还额度都要过那条交付队列，因此这一条既量稳态成本，也顺带钉住
     *          「换成不空载的队列之后，每请求的分配没有变多」
     */
    TEST(QuicStreamLayerAllocations, ReceiveDeliverAndReleaseAllocations)
    {
        const std::vector<std::uint8_t> body(16, 'r');
        const auto runOnce = [&body]() -> std::size_t
        {
            QuicStreamLayer layer(makeStreamLayerParameters());

            QuicStreamFrame frame;
            frame.streamId = 0;
            frame.offset   = 0;
            frame.data     = std::span<const std::uint8_t>(body);
            frame.isFinal  = true;
            static_cast<void>(layer.onStreamFrame(frame));

            std::size_t deliveredByteCount = 0;
            while (layer.hasDeliveries())
            {
                if (const std::optional<QuicStreamDelivery> delivery = layer.takeDelivery(); delivery.has_value())
                {
                    deliveredByteCount += delivery->bytes.size();
                }
            }
            layer.releaseReceiveWindow(frame.streamId, deliveredByteCount);
            return deliveredByteCount;
        };
        ASSERT_EQ(runOnce(), body.size()) << "这条形状没把正文交付完，读数测的不是稳态";

        const AllocationProfile profile = measurePerOperation(runOnce);
        EXPECT_EQ(profile.resultSum, kMeasurementIterations * body.size()) << "有几次没交付完，读数不可信";
        std::printf("quic 流层每条连接「建起来 + 收一条请求」成本 %llu 次分配 / %llu 字节\n",
                    static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.bytesPerOperation));
    }
} // namespace AsynGyanis::Net
