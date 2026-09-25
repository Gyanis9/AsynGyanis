#!/usr/bin/env bash
# 用 libFuzzer 持续模糊四类协议解码器（Windows 与 Linux 两侧的可复现跑法）。
#
# 为什么不是「把项目里的 Net.lib 拿来链」：LLVM 官方 Windows 包里 compiler-rt 的 fuzzer 运行时是
# 按**静态 CRT**（/MT）编的，而本仓的 MSVC 构建一律是 /MD——lld-link 会以
# `/failifmismatch: RuntimeLibrary` 直接拒绝混链。所以这里只编「模糊内核 + 四个解码器 + 它们的
# 真实依赖闭包」这一小组翻译单元，整份产物自洽为 /MT，不碰项目里任何一份现成库。
# 闭包是一项一项按链接器报的未定义符号补齐的（最后一项是 HttpDate 需要的 PlatformTime::utcTime），
# 因此这份清单本身就是「模糊目标到底依赖哪些代码」的答案。Linux 侧没有 /MT 这层约束（CI runner 自带
# 带 fuzzer 运行时的 clang），但沿用同一份清单：两侧模糊的是同一批代码，换编译器不该换覆盖面。
#
# 跑法：
#   scripts/fuzz-net.sh                 # 默认 60 秒
#   scripts/fuzz-net.sh 600             # 十分钟一轮
#   CLANG_CL=D:/llvm/bin/clang-cl.exe scripts/fuzz-net.sh 300   # Windows 侧自己指路
#   CXX=clang++-18 scripts/fuzz-net.sh 300                       # Linux 侧换一个 clang
# 额外的 libFuzzer 参数按位置透传，例如 -jobs=4 -workers=4。
#
# 违例（不变量被破坏、崩溃、越界写）会以 abort 收场，并把输入写成 .fuzz/artifacts/crash-*；
# 把那份输入原样搬进 tests/Net/Fuzz/TestProtocolFuzz.cpp 的种子用例即可常驻。
set -uo pipefail

projectRoot="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${projectRoot}" || exit 1

durationSeconds="${1:-60}"
shift || true

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
    "src/Net/Http/HttpDate.cpp"
    "src/Base/Exception/Exception.cpp"
    "src/Base/Exception/InvalidArgumentException.cpp"
    "src/Base/Exception/LogicException.cpp"
    "src/Base/Exception/StackTrace.cpp"
    "src/Platform/System/PlatformTime.cpp"
)

if [[ "$(uname -s)" == Linux* ]]; then
    compiler="${CXX:-clang++}"
    if ! command -v "${compiler}" >/dev/null 2>&1; then
        echo "找不到 ${compiler}：Linux 侧要一份带 compiler-rt fuzzer 库的 clang（apt install clang 即可）" >&2
        exit 1
    fi
    objDir="${projectRoot}/.fuzz/obj"
    binary="${projectRoot}/.fuzz/netfuzz"
    artifacts="${projectRoot}/.fuzz/artifacts"
    # address 与 fuzzer 同时开：这批解码器的历史缺陷全是越界与释放后读，光靠 libFuzzer 自己看不出来。
    # 帧指针留着，崩溃栈才带得出行号
    compileFlags=(-std=c++2b -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address -Isrc -Itests/Net)
    linkFlags=(-fsanitize=fuzzer,address)
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

    echo "开跑 ${durationSeconds} 秒（附加参数透传给 libFuzzer）"
    cd "${projectRoot}/.fuzz" || exit 1
    ./netfuzz "-max_total_time=${durationSeconds}" -rss_limit_mb=2048 -timeout=15 \
        "-artifact_prefix=${artifacts}/" -print_final_stats=1 "$@"
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
compileFlags=(/std:c++latest /O1 /MT /DNDEBUG /EHsc -fsanitize=fuzzer /Isrc /Itests/Net)
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

echo "开跑 ${durationSeconds} 秒（附加参数透传给 libFuzzer）"
cd .fuzz || exit 1
./netfuzz.exe "-max_total_time=${durationSeconds}" -rss_limit_mb=2048 \
    "-artifact_prefix=${artifacts}/" -print_final_stats=1 "$@"
