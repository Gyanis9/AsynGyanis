// 自研协议解码器的属性化随机模糊：四类解码器 × 六条不变量，固定种子保证失败可复现。
// 与 libFuzzer 入口（FuzzTargets.cpp）共用同一份内核，因此「本地跑绿」与「CI 持续模糊」判的是同一件事。
//
// 粘滞、复位与「失败不留半截产出」这几条判据已按突变法证过非恒绿（2026-09-24，各自临时改坏实现后转红、
// 复原后转绿；每条都报出了轮次、种子与转义输入）：
//   · WebSocket I4/I6「错误态没粘住，或还在消费字节」——去掉 parse 开头的 Failed 早退
//   · WebSocket I5「reset() 后合法输入解不出帧」——让 reset() 漏清 m_stage（留在 Failed）
//   · Http2  I4/I6 同上——把 Failed 的结论洗成 NeedMore（顺带一条事实：改成「重回帧头阶段」会让解码器
//     以 std::array 越界的硬断言收场，说明这条驱动确实喂到了状态机深处，而不是在门口打转）
//   · Http3  I4「复查 nextFrame 不再报错」——把粘滞错误改回「还差字节」
//   · Hpack  I6「decode 失败却留下了字段」——去掉失败路径上的 headerFields.clear()
// 两处口径要记下：① 只把 h3 的早退条件改成「几乎不成立」并不会转红，因为同样的坏字节会被重新判成
// 同一个错——这条钉的是「不再报错」这个可观测面，不是内部那个标记位；② HPACK 的 I5 与其它三条同形状，
// 没有单独再突变一次。
#include "Fuzz/ProtocolFuzzKernel.h"
#include "CommonTestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace AsynGyanis::Net::Fuzz
{
    namespace
    {
        /// 每个目标的随机轮数默认值。每轮都含「整体喂 + 逐字节喂」两路驱动与一次复位探针。
        /// 实测（Debug+ASan，本机）：400 轮时四条用例合计 0.84 秒，2 万轮合计 15 秒——
        /// 常驻防线取 400 轮图的是「每次都跑得完」，要加压就设 ASYN_FUZZ_ROUNDS（判同一批不变量，
        /// 且输入序列是默认跑的前缀）。上限 20 万轮：再多是 CI 超时而不是模糊。
        /// 另一条来自这里的教训：驱动不推进时，2 万轮能把进程撑到 4.49 GB 常驻（同一段字节被反复重喂），
        /// maximumDriveSteps 那道闸就是为这种「模糊器比被测物先垮」而加的
        constexpr int kDefaultRandomRoundCount = 400;

        /// 轮数上限：按 2 万轮 15 秒实测，20 万轮约 2.5 分钟（四条合计），够一次深度跑；
        /// 再多就是把作业变成超时，而不是把覆盖变深
        constexpr int kMaximumRandomRoundCount = 200000;

        /// 防空转断言生效所需的最小轮数。那两条断言判的是「整串输入的整体性质」（既解出过帧、也判过次错），
        /// 一两个样本不足以说明生成器退化——第 0 轮的输入恰好解不出帧是正常现象，不是缺陷。
        /// 因此低于这个轮数就**明说不判**，而不是把钳制下限抬到恰好能过的那个数
        constexpr int kAntiIdleMinimumRoundCount = 100;

        /**
         * @brief 取本轮实际用的轮数：ASYN_FUZZ_ROUNDS 覆盖默认值
         * @details 常驻跑 400 轮、临时加压用 `ASYN_FUZZ_ROUNDS=20000 ./TestNet --gtest_filter='ProtocolFuzz.*'`，
         *          两条路判的是同一批不变量——所以「本地撞出来的」与「CI 拦下来的」不会各说各话。
         *          变量缺失、空、含非数字、超出上限都回落到默认值或饱和：这是加压旋钮，不是被测契约，
         *          填错不该让用例变红（也因此不走 std::stoul——它对全数字但超范围的串会抛异常）。
         *          把轮数调到 kAntiIdleMinimumRoundCount 以下时，防空转那两条断言按下面的说明不判
         * @return int 生效轮数
         */
        int randomRoundCount()
        {
            const std::optional<std::string> overrideText = AsynGyanis::TestSupport::readEnvironmentVariable("ASYN_FUZZ_ROUNDS");
            if (!overrideText.has_value() || overrideText->empty())
            {
                return kDefaultRandomRoundCount;
            }

            std::uint64_t accumulated = 0;
            for (const char digit: *overrideText)
            {
                if (digit < '0' || digit > '9')
                {
                    return kDefaultRandomRoundCount;
                }
                // 饱和累加：再大的数字也只是「跑到上限」，不会被回绕成一个小值而悄悄少跑
                accumulated = std::min<std::uint64_t>(accumulated * 10U + static_cast<std::uint64_t>(digit - '0'),
                                                      static_cast<std::uint64_t>(kMaximumRandomRoundCount));
            }
            return accumulated == 0U ? kDefaultRandomRoundCount : static_cast<int>(accumulated);
        }

        /// 单份输入的长度上限
        constexpr std::size_t kMaximumInputLength = 512;

        /// 各目标的固定种子：失败信息里带轮次与轮数，凭种子就能在同一台机器上重放同一串输入。
        /// 随机源是单调流，因此加压跑（ASYN_FUZZ_ROUNDS 调大）的前 N 轮与默认跑完全同序——
        /// 默认跑绿而加压跑红的轮次，把前面的输入原样搬进用例即可常驻
        constexpr std::uint64_t kSeedWebSocket = 20260924ULL;
        constexpr std::uint64_t kSeedHttp2     = 20260925ULL;
        constexpr std::uint64_t kSeedHttp3     = 20260926ULL;
        constexpr std::uint64_t kSeedHpack     = 20260927ULL;

        /**
         * @brief 对某个目标跑一轮随机模糊
         * @param target 目标解码器
         * @param seed 随机源种子
         */
        void runRandomRounds(const Target target, const std::uint64_t seed)
        {
            DeterministicRandom random(seed);
            std::size_t producedFrameCount = 0;
            std::size_t rejectedRoundCount = 0;
            const int   roundCount         = randomRoundCount();

            for (int round = 0; round < roundCount; ++round)
            {
                const std::string input = makeInput(target, random, kMaximumInputLength);
                RunStats stats;
                const std::string violation = checkInvariants(target, input, &stats);
                if (!violation.empty())
                {
                    FAIL() << targetName(target) << " 第 " << round << " 轮违例：" << violation
                           << "，输入（转义）=" << toEscapedText(input) << "，种子=" << seed << "，轮数=" << roundCount;
                }
                producedFrameCount += stats.producedFrameCount;
                rejectedRoundCount += stats.isErrorEnd ? 1U : 0U;
            }

            // 防空转：既要真解出过帧，也要真判过错。只有前者说明输入太规整（打不到拒绝分支），
            // 只有后者说明输入生成器坏了（一路喂垃圾）——两种都让这套用例失去意义。
            // 低于 kAntiIdleMinimumRoundCount 时不判：那是整体性质，几轮样本撑不起结论
            if (roundCount >= kAntiIdleMinimumRoundCount)
            {
                EXPECT_GT(producedFrameCount, 0U) << targetName(target) << " 全程没解出过一帧：输入生成器或驱动退化";
                EXPECT_GT(rejectedRoundCount, 0U) << targetName(target) << " 全程没判过一次错：输入太规整，拒绝分支没被覆盖";
            }
        }

        /**
         * @brief 对某个目标跑截断矩阵：合法输入的每一个前缀都必须「可解或还要数据」，不得崩、不得产出半截帧
         * @details 增量解码器的历史缺陷集中在截断点上（切在掩码键中间、帧头中间、长度域中间），
         *          前缀是打中这些位置最经济的方式
         */
        void runTruncationMatrix(const Target target)
        {
            const std::string valid = validInput(target);
            ASSERT_FALSE(valid.empty()) << targetName(target) << " 的合法输入样本为空，本用例无从判定";

            for (std::size_t length = 0; length <= valid.size(); ++length)
            {
                const std::string truncated = valid.substr(0, length);
                const std::string violation = checkInvariants(target, truncated);
                EXPECT_TRUE(violation.empty()) << targetName(target) << " 截断到 " << length << " 字节时违例：" << violation
                                              << "，输入（转义）=" << toEscapedText(truncated);
            }
        }
    } // namespace

    TEST(ProtocolFuzz, WebSocketFrameDecoderKeepsInvariants)
    {
        runRandomRounds(Target::WebSocketFrame, kSeedWebSocket);
        runTruncationMatrix(Target::WebSocketFrame);
    }

    TEST(ProtocolFuzz, Http2FrameDecoderKeepsInvariants)
    {
        runRandomRounds(Target::Http2Frame, kSeedHttp2);
        runTruncationMatrix(Target::Http2Frame);
    }

    TEST(ProtocolFuzz, Http3FrameReaderKeepsInvariants)
    {
        runRandomRounds(Target::Http3Frame, kSeedHttp3);
        runTruncationMatrix(Target::Http3Frame);
    }

    TEST(ProtocolFuzz, HpackDecoderKeepsInvariants)
    {
        runRandomRounds(Target::HpackBlock, kSeedHpack);
        runTruncationMatrix(Target::HpackBlock);
    }
} // namespace AsynGyanis::Net::Fuzz
