#!/usr/bin/env bash
# 用 libFuzzer 持续模糊四类协议解码器（Windows 与 Linux 两侧的可复现跑法）。
#
# 为什么不是「把项目里的 Net.lib 拿来链」：LLVM 官方 Windows 包里 compiler-rt 的 fuzzer 运行时是
# 按**静态 CRT**（/MT）编的，而本仓的 MSVC 构建一律是 /MD——lld-link 会以
# `/failifmismatch: RuntimeLibrary` 直接拒绝混链。所以这里只编「模糊内核 + 四个解码器 + 它们的
# 真实依赖闭包」这一小组翻译单元，整份产物自洽为 /MT，不碰项目里任何一份现成库。
# 闭包是一项一项按链接器报的未定义符号补齐的（最后一项是 HttpDate 需要的 PlatformTime::utcTime），
# 因此这份清单本身就是「模糊目标到底依赖哪些代码」的答案。Linux 侧没有 /MT 这层约束，但沿用同一份
# 清单：两侧模糊的是同一批代码，换编译器不该换覆盖面。那一侧的约束换成了 clang 的版本号（libstdc++
# 的 <expected> 要 __cpp_concepts >= 202002L，clang 19 起才给；fuzzer 运行时又只按 libstdc++ 编，
# 换 libc++ 会在链接期炸），所以脚本按候选编译器挨个探，一个都不成时直接说出要装什么。
#
# 跑法：
#   scripts/fuzz-net.sh                 # 默认：每一档目标各跑 60 秒（各一份语料与日志）
#   scripts/fuzz-net.sh 600             # 每类十分钟
#   ASYN_FUZZ_TARGETS=combined scripts/fuzz-net.sh 60       # 回到单次混合跑（各档轮转共用预算）
#   ASYN_FUZZ_TARGETS="HpackBlock Http3Frame" scripts/fuzz-net.sh 120   # 只跑其中两档
#   CLANG_CL=D:/llvm/bin/clang-cl.exe scripts/fuzz-net.sh 300   # Windows 侧自己指路
#   CXX=clang++-19 scripts/fuzz-net.sh 300                       # Linux 侧换一个 clang（要 19 以上）
#   ASYN_FUZZ_INCLUDE_DIRS="D:/conan/p/nlohma.../p/include"  scripts/fuzz-net.sh 300
#       # nlohmann/json 的头目录不在本仓里，Conan 环境没进 INCLUDE 时用它指路（Windows 侧尤其）
# 额外的 libFuzzer 参数按位置透传，例如 -jobs=4 -workers=4。
#
# 为什么默认按目标拆开跑：混合跑只有一份总预算，摊到每档的目标只剩几分之一，
# 而且「其中一档根本没被推到」从总执行数上完全看不出来。拆开后每档的账落在
# `.fuzz/log/<目标>.log` 末尾那行 `FUZZ-TARGET-CALLS …`，语料各自存进 `.fuzz/corpus/<目标>/`
# 语料只有在这台机器上接着跑才会长——CI 每次都是干净工作区，从空语料开始；它留 14 天制品是给「把撞出来的形状喂回本地继续挖」和「搬进 gtest 种子」用的，不是增量缓存。
#
# 违例（不变量被破坏、崩溃、越界写）会以 abort 收场，并把输入写成 .fuzz/artifacts/crash-*；
# 把那份输入原样搬进 tests/Net/Fuzz/TestProtocolFuzz.cpp 的种子用例即可常驻。
set -uo pipefail

projectRoot="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${projectRoot}" || exit 1

durationSeconds="${1:-60}"
shift || true

# 目标清单：默认每档各跑一整轮（每档预算都是 durationSeconds），这样「哪档在推进」看得见。
# 传 ASYN_FUZZ_TARGETS=combined 退回单次混合跑（一个进程轮转全部目标），本机快速冒烟用得上。
# 每档的语料与日志各落一份：语料是「下次能接着挖」的起点，日志是给 CI 数执行次数用的。
targetSelection="${ASYN_FUZZ_TARGETS:-WebSocketFrame Http2Frame Http3Frame HpackBlock QuicPacket QuicFrameSequence QuicParameters}"
corpusRoot="${ASYN_FUZZ_CORPUS_DIR:-${projectRoot}/.fuzz/corpus}"
logRoot="${projectRoot}/.fuzz/log"

# 把 ASYN_FUZZ_TARGETS 展开成数组；combined 是「不指定目标」的哨兵
runTargets=()
if [[ "${targetSelection}" != "combined" ]]; then
    read -ra runTargets <<<"${targetSelection}"
fi

# 单轮跑法：指定目标时把选择钉死（其余各档必须零调用，这条判据在 CI 侧核），并各自留语料与日志
runOneTarget() {
    local binary="$1" artifacts="$2" targetName="$3"
    shift 3
    local corpusDir="${corpusRoot}" logFile=""
    if [[ -n "${targetName}" ]]; then
        corpusDir="${corpusRoot}/${targetName}"
        logFile="${logRoot}/${targetName}.log"
        mkdir -p "${corpusDir}" "${logRoot}"
    else
        mkdir -p "${corpusDir}" "${logRoot}"
        logFile="${logRoot}/combined.log"
    fi
    echo "开跑 ${targetName:-混合四类} ${durationSeconds} 秒（附加参数透传给 libFuzzer）"
    (
        cd "${projectRoot}/.fuzz" || exit 1
        if [[ -n "${targetName}" ]]; then
            env ASYN_FUZZ_TARGET="${targetName}" "${binary}" "-max_total_time=${durationSeconds}" \
                -rss_limit_mb=2048 -timeout=15 "-artifact_prefix=${artifacts}/" \
                -print_final_stats=1 "${corpusDir}" "$@"
        else
            "${binary}" "-max_total_time=${durationSeconds}" \
                -rss_limit_mb=2048 -timeout=15 "-artifact_prefix=${artifacts}/" \
                -print_final_stats=1 "${corpusDir}" "$@"
        fi
    ) 2>&1 | tee "${logFile}"
    # 上面那个管道把模糊器的退出码换成了 tee 的：判「跑没跑成」必须回头看 PIPESTATUS[0]
    return "${PIPESTATUS[0]}"
}

# 跑完清单上的每一档：任何一档判失败就整体失败，但其余各档照样跑完（一次红要看全每一档）
runAllTargets() {
    local binary="$1" artifacts="$2" status=0
    shift 2
    if [[ ${#runTargets[@]} -eq 0 ]]; then
        runOneTarget "${binary}" "${artifacts}" "" "$@" || status=$?
        return "${status}"
    fi
    local target
    for target in "${runTargets[@]}"; do
        runOneTarget "${binary}" "${artifacts}" "${target}" "$@" || status=$?
    done
    return "${status}"
}

# 模糊入口 + 四个解码器 + 依赖闭包（少一项就链不出来）
sources=(
    "tests/Net/Fuzz/FuzzTargets.cpp"
    "tests/Net/Fuzz/ProtocolFuzzKernel.cpp"
    "src/Net/WebSocket/WebSocketFrame.cpp"
    "src/Net/Http2/Http2Frame.cpp"
    "src/Net/Http2/Hpack.cpp"
    "src/Net/Http3/Http3Frame.cpp"
    "src/Net/Quic/Codec/QuicVariableLengthInteger.cpp"
    "src/Net/Http/HttpHeaderFieldStore.cpp"
    "src/Net/Http/HttpRequest.cpp"
    # 请求对象的两条成员函数各自落在自己的文件里：cookies() 要 HttpCookie.cpp、multipartForm()
    # 要 MultipartForm.cpp，少哪一个都在链接期报未定义（清单本身就是按链接器的报错补齐的）
    "src/Net/Http/HttpCookie.cpp"
    "src/Net/Http/MultipartForm.cpp"
    "src/Net/Http/HttpDate.cpp"
    "src/Base/Exception/Exception.cpp"
    # 三条异常链的抛出点与调用栈上收成一份载荷基之后，Exception.cpp 只剩转调：那份载荷的构造函数
    # 定义在自己的 .cpp 里，不列进来链接期就报 ExceptionPayload 未定义（fuzz 作业实测红过一轮）
    "src/Base/Exception/ExceptionPayload.cpp"
    "src/Base/Exception/InvalidArgumentException.cpp"
    "src/Base/Exception/LogicException.cpp"
    "src/Base/Exception/StackTrace.cpp"
    "src/Platform/System/PlatformTime.cpp"
)

if [[ "$(uname -s)" == Linux* ]]; then
    objDir="${projectRoot}/.fuzz/obj"
    binary="${projectRoot}/.fuzz/netfuzz"
    artifacts="${projectRoot}/.fuzz/artifacts"
    # 编译器与语言档要探出来，不能写死。两处都是实测出来的坑：
    #   · std::expected：libstdc++ 的那份头文件守卫要求 __cpp_concepts >= 202002L，clang 18 把它定在
    #     201907L，于是 clang 18 + libstdc++ 在任何 -std 下都看不见这个类型（报错落在各个头文件里，
    #     读起来像代码坏了）；clang 19 起才满足。
    #   · compiler-rt 的 fuzzer 运行时是按 libstdc++ 编的：想用 -stdlib=libc++ 绕开上一条，链接期会
    #     报一串 std::__cxx11 未定义（实测），所以标准库不能换，只能换 clang 的版本。
    # 各台机器与 runner 上默认的 clang++ 是哪一档不可预知，就按候选挨个试到「编得过也链得上」为止
    if [[ -n "${CXX:-}" ]]; then
        compilerCandidates=("${CXX}")
    else
        compilerCandidates=(clang++ clang++-21 clang++-20 clang++-19)
    fi
    probeSource="$(mktemp "${TMPDIR:-/tmp}/asyn-fuzz-probe-XXXXXX.cpp")"
    probeBinary="$(mktemp "${TMPDIR:-/tmp}/asyn-fuzz-probe-XXXXXX")"
    # 探针取 libFuzzer 的入口形状：它自己不带 main，链接阶段才真的把 runtime 拽进来验 ABI 是否同侧
    printf '#include <cstddef>\n#include <cstdint>\n#include <expected>\n#include <stop_token>\n\nextern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *const data, const std::size_t size)\n{\n    std::expected<int, int> value{1};\n    std::stop_source          source;\n    return value.has_value() && source.stop_possible() && size > 0U && data != nullptr ? 0 : 1;\n}\n' >"${probeSource}"
    compiler=""
    chosenFlags=""
    for candidateCompiler in "${compilerCandidates[@]}"; do
        command -v "${candidateCompiler}" >/dev/null 2>&1 || continue
        for standard in -std=c++2b -std=c++23; do
            if "${candidateCompiler}" "${standard}" -fsanitize=fuzzer,address "${probeSource}" -o "${probeBinary}" >/dev/null 2>&1; then
                compiler="${candidateCompiler}"
                chosenFlags="${standard}"
                break 2
            fi
        done
    done
    rm -f "${probeSource}" "${probeBinary}"
    if [[ -z "${compiler}" ]]; then
        echo "没有一档 clang 能同时给出 std::expected 与 std::stop_source 并链上 compiler-rt 的 fuzzer" >&2
        echo "运行时，而本仓四类解码器的接口两头都靠它们。libstdc++ 的 <expected> 要求 clang 报" >&2
        echo "__cpp_concepts >= 202002L，clang 18 及以下不满足：apt install clang-19 libfuzzer-19-dev，" >&2
        echo "或用 CXX=<路径> 指一份 clang 19 以上的编译器" >&2
        exit 1
    fi
    echo "编译器 ${compiler}，编译档 ${chosenFlags}（实测能编能链 std::expected 与 std::stop_source）"
    # 闭包里有一处仓外头文件：HttpRequest::jsonBody() 的返回类型就是 Base::ConfigValue，而那个类型
    # 包着 nlohmann::json。目录从 ASYN_FUZZ_INCLUDE_DIRS 给（空格分隔），Linux 侧装了
    # nlohmann-json3-dev 就不用给（它在默认搜索路径里）。先探一次再开编：缺头文件会在第十几个
    # 翻译单元上才报「file not found」，读起来像清单写错了
    read -ra nlohmannIncludeDirs <<<"${ASYN_FUZZ_INCLUDE_DIRS:-}"
    nlohmannIncludes=()
    for includeDir in "${nlohmannIncludeDirs[@]}"; do
        nlohmannIncludes+=("-I${includeDir}")
    done
    if ! printf '#include <nlohmann/json.hpp>\n\nint main()\n{\n    return 0;\n}\n' |
        "${compiler}" "${chosenFlags}" "${nlohmannIncludes[@]}" -fsyntax-only -x c++ - >/dev/null 2>&1; then
        echo "看不见 <nlohmann/json.hpp>，而 Base::ConfigValue 就建在它上面。Linux 侧 apt install" >&2
        echo "nlohmann-json3-dev；否则用 ASYN_FUZZ_INCLUDE_DIRS 把它的 include 目录传进来" >&2
        exit 1
    fi
    # address 与 fuzzer 同时开：这批解码器的历史缺陷全是越界与释放后读，光靠 libFuzzer 自己看不出来。
    # 帧指针留着，崩溃栈才带得出行号。编译与链接用同一个 ${chosenFlags}：标准库两侧必须同一条，
    # 上面那次「能编也能链」的探针判的就是这一条
    compileFlags=("${chosenFlags}" -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address -Isrc -Itests/Net "${nlohmannIncludes[@]}")
    linkFlags=("${chosenFlags}" -fsanitize=fuzzer,address)
    objectSuffix="o"

    mkdir -p "${objDir}" "${artifacts}"
    objects=()
    for source in "${sources[@]}"; do
        objectName="${objDir}/$(basename "${source}" .cpp).${objectSuffix}"
        echo "编译 ${source}"
        "${compiler}" "${compileFlags[@]}" -c "${source}" -o "${objectName}" || exit 1
        # 没产出目标文件就等于这一项根本没进链接清单：当场停下，别留到链接期报一串「找不到 .o」
        if [[ ! -f "${objectName}" ]]; then
            echo "编译 ${source} 之后没有产出 ${objectName}" >&2
            exit 1
        fi
        objects+=("${objectName}")
    done

    echo "链接 ${binary}"
    "${compiler}" "${linkFlags[@]}" -o "${binary}" "${objects[@]}" || exit 1

    runAllTargets "${binary}" "${artifacts}" "$@"
    exit $?
fi

# clang-cl 与 lld-link 收 Windows 形态路径，但 Git Bash 的 /g/... 它们不认；pwd -W 给出的
# 「G:/Codes/...」带盘符又是正斜杠，两边都收，省掉对 cygpath 的依赖
rootForClang="$(pwd -W)"
objDir="${rootForClang}/.fuzz/obj"
binary="${rootForClang}/.fuzz/netfuzz.exe"
artifacts="${rootForClang}/.fuzz/artifacts"

# 定位 clang-cl：优先环境变量，其次本机 llvm.org 的默认安装位置
clangCl="${CLANG_CL:-}"
if [[ -z "${clangCl}" ]]; then
    for candidate in /g/Tools/LLVM/bin/clang-cl.exe "/c/Program Files/LLVM/bin/clang-cl.exe"; do
        if [[ -x "${candidate}" ]]; then
            clangCl="${candidate}"
            break
        fi
    done
fi
if [[ -z "${clangCl}" ]]; then
    echo "找不到 clang-cl：装一份带 compiler-rt fuzzer 库的 LLVM，或用 CLANG_CL=<路径> 指路" >&2
    exit 1
fi

mkdir -p "${objDir}" "${artifacts}"

# 以 '/' 开头的参数会被 MSYS 改写，因此每次调用都显式关掉路径转换；/MT 的理由见文件头
# nlohmann/json.hpp 不在本仓里（Base::ConfigValue 包着它，而 HttpRequest.cpp 用到那个类型）。
# Windows 侧没有系统级的 include 路径可退，用 ASYN_FUZZ_INCLUDE_DIRS 把 Conan 包好的那份目录传进来，
# 空格分隔，例如 ASYN_FUZZ_INCLUDE_DIRS="$CONAN_HOME/p/nlohmd<哈希>/p/include"
read -ra nlohmannIncludeDirs <<<"${ASYN_FUZZ_INCLUDE_DIRS:-}"
nlohmannIncludes=()
for includeDir in "${nlohmannIncludeDirs[@]}"; do
    nlohmannIncludes+=("/I${includeDir}")
done
compileFlags=(/std:c++latest /O1 /MT /DNDEBUG /EHsc -fsanitize=fuzzer /Isrc /Itests/Net "${nlohmannIncludes[@]}")
objects=()
for source in "${sources[@]}"; do
    objectName="$(basename "${source}" .cpp).obj"
    echo "编译 ${source}"
    MSYS_NO_PATHCONV=1 "${clangCl}" "${compileFlags[@]}" /c "/Fo${objDir}/${objectName}" "${source}" || exit 1
    # 没产出目标文件就等于这一项根本没进链接清单：当场停下，别留到链接期报一串「找不到 .obj」
    if [[ ! -f "${objDir}/${objectName}" ]]; then
        echo "编译 ${source} 之后没有产出 ${objDir}/${objectName}" >&2
        exit 1
    fi
    objects+=("${objDir}/${objectName}")
done

echo "链接 ${binary}"
MSYS_NO_PATHCONV=1 "${clangCl}" -fsanitize=fuzzer "/Fe${binary}" "${objects[@]}" || exit 1

MSYS_NO_PATHCONV=1 runAllTargets "${binary}" "${artifacts}" "$@"
exit $?
