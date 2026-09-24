// libFuzzer 入口：把同一个内核交给覆盖引导的模糊器跑。仅在 -DASYN_BUILD_FUZZ_TARGETS=ON 时构建。
//
// 工具链现状（2026-09-24 实查，别再重走）：本机的 llvm-mingw clang 22.1.7 对 x86_64-w64-windows-gnu
// 目标直接拒绝 -fsanitize=fuzzer 与 -fsanitize=fuzzer-no-link，常用容器里没装 clang——因此这条路只能
// 在装了 clang 的 Linux 上跑（CI 的 fuzz 作业自己 apt 装）。这份入口的代码本身已过 clang 的
// -fsyntax-only（连同四个解码器 TU 一起全绿），换到 Linux 上缺的只是模糊器运行时，不是可编性。
//
// 与 gtest 那套的分工：gtest 用例是常驻防线（固定种子、每次全量跑、判同一批不变量），
// 这里是持续探索（输入由模糊器按覆盖率反馈生成，撞出来的语料落盘后可回填进 gtest 的种子用例）。
#include "Fuzz/ProtocolFuzzKernel.h"

#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <string>

namespace
{
    /// libFuzzer 每次给出的字节按 256 种取值轮转分配给四类解码器，一次运行即可同时推进四个目标
    constexpr std::size_t kTargetCount = static_cast<std::size_t>(AsynGyanis::Net::Fuzz::Target::Count);
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (data == nullptr || size == 0U || size > 4096U)
    {
        return 0;
    }

    // 用输入自身的第一字节选目标：语料最小化时每个目标都会各自被保留一份代表输入。
    // 注意规模差异——模糊器一次运行能跑到百万级输入，正是 gtest 那套在插桩构建里跑不动的量
    const std::size_t targetIndex = static_cast<std::size_t>(data[0]) % kTargetCount;
    const std::string input(reinterpret_cast<const char *>(data) + 1U, size - 1U);

    // 违例就主动 abort：模糊器只对崩溃/异常做最小化与存证，把它当「返回了但没通过」是看不见的。
    // abort 之后 libFuzzer 会把这份输入写成 crash 用例，回填进 gtest 侧的种子即可常驻
    if (const std::string violation = AsynGyanis::Net::Fuzz::checkInvariants(
                static_cast<AsynGyanis::Net::Fuzz::Target>(targetIndex), input); !violation.empty())
    {
        std::abort();
    }
    return 0;
}
