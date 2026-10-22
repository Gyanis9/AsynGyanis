#!/usr/bin/env bash
# 用 libFuzzer 持续模糊各档协议解码器（Windows 与 Linux 两侧的可复现跑法）。
# 产物自洽为 /MT：LLVM 官方 Windows 包里 compiler-rt 的 fuzzer 运行时按静态 CRT（/MT）编，
# 本仓 MSVC 构建一律 /MD，lld-link 会以 /failifmismatch: RuntimeLibrary 拒绝混链。

# 所以只编「模糊内核 + 各档解码器 + 真实依赖闭包」这一小组翻译单元，不碰项目里任何现成库；
# 闭包按链接器报的未定义符号逐项补齐，这份清单就是「模糊目标依赖哪些代码」的答案。

# Linux 侧没有 /MT 约束，但沿用同一份清单：两侧模糊同一批代码，换编译器不该换覆盖面。
# 那一侧的要求是 clang 19 以上：libstdc++ 的 <expected> 要 __cpp_concepts >= 202002L，
# 而 compiler-rt 的 fuzzer 运行时只按 libstdc++ 编，换 libc++ 会在链接期炸。

# 脚本按候选编译器挨个探，一个都不成时直接说出要装什么。

# 跑法：
#   scripts/fuzz-net.sh                 # 默认：每一档目标各跑 60 秒（各一份语料与日志）
#   scripts/fuzz-net.sh 600             # 每类十分钟

#   ASYN_FUZZ_TARGETS=combined scripts/fuzz-net.sh 60       # 单次混合跑（各档轮转共用预算）
#   ASYN_FUZZ_TARGETS="HpackBlock Http3Frame" scripts/fuzz-net.sh 120   # 只跑其中两档
#   CLANG_CL=D:/llvm/bin/clang-cl.exe scripts/fuzz-net.sh 300   # Windows 侧自己指路

#   CXX=clang++-19 scripts/fuzz-net.sh 300                       # Linux 侧换 clang（要 19 以上）
#   ASYN_FUZZ_INCLUDE_DIRS="D:/conan/p/nlohma.../p/include"  scripts/fuzz-net.sh 300
#       上一行给 nlohmann/json 头目录：它不在本仓，Conan 环境没进 INCLUDE 时用它指路。

#   ASYN_FUZZ_ZLIB_DIR="D:/conan/p/zlib.../p" scripts/fuzz-net.sh 300
#       permessage-deflate 档包着 zlib：Linux 有系统 zlib 就不用给（链 -lz）；
#       Windows clang-cl 侧用它指包目录（取 <目录>/include 与 <目录>/lib/zlib.lib）。

# 额外的 libFuzzer 参数按位置透传，例如 -jobs=4 -workers=4。

# 为什么默认按目标拆开跑：混合跑只有一份总预算，摊到每档只剩几分之一，
# 而且「其中一档根本没被推到」从总执行数上完全看不出来。

# 拆开后每档的账落在 `.fuzz/log/<目标>.log` 末尾那行 `FUZZ-TARGET-CALLS …`，
# 语料各自存进 `.fuzz/corpus/<目标>/`；CI 每次从空语料开始，14 天制品是给
# 「把撞出来的形状喂回本地继续挖」和「搬进 gtest 种子」用的，不是增量缓存。

# 违例（不变量被破坏、崩溃、越界写）以 abort 收场，输入写成 .fuzz/artifacts/crash-*；
# 把那份输入原样搬进 tests/Net/Fuzz/TestProtocolFuzz.cpp 的种子用例即可常驻。
set -uo pipefail

projectRoot="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${projectRoot}" || exit 1

durationSeconds="${1:-60}"
shift || true

# 目标清单：默认每档各跑一整轮（每档预算都是 durationSeconds），这样「哪档在推进」看得见。
# 传 ASYN_FUZZ_TARGETS=combined 退回单次混合跑（一个进程轮转全部目标），本机快速冒烟用得上。
# 每档的语料与日志各落一份：语料是「下次能接着挖」的起点，日志是给 CI 数执行次数用的。
targetSelection="${ASYN_FUZZ_TARGETS:-Http1Request WebSocketFrame Http2Frame Http3Frame HpackBlock QpackBlock PerMessageDeflate HttpResponse QuicPacket QuicFrameSequence QuicParameters JwtCompact ConfigValueText LogEscaping}"
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
    echo "开跑 ${targetName:-混合全部目标} ${durationSeconds} 秒（附加参数透传给 libFuzzer）"
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

# 模糊入口 + 全部解码器档 + 依赖闭包（少一项就链不出来）
sources=(
    "tests/Net/Fuzz/FuzzTargets.cpp"
    "tests/Net/Fuzz/ProtocolFuzzKernel.cpp"
    "src/Net/WebSocket/WebSocketFrame.cpp"
    # WebSocketFrame.cpp 的 Close 判定要按 UTF-8 校验原因短语（分类器在 WebSocketUtf8.cpp）：
    # 漏了这一项链不过，链接期报 findInvalidWebSocketUtf8ByteOffset 未定义。
    "src/Net/WebSocket/WebSocketUtf8.cpp"
    "src/Net/Http2/Http2Frame.cpp"
    "src/Net/Http2/Hpack.cpp"
    "src/Net/Http3/Http3Frame.cpp"
    # QPACK 档：解码器本体 + 它复用的 HPACK 整数/字符串助手（后者已在上面），另有错误码映射
    "src/Net/Http3/Qpack.cpp"
    "src/Net/Http3/Http3Error.cpp"
    # permessage-deflate 档：协商解析与压缩/解压的包装。它包着 zlib（`<zlib.h>`），是本闭包里
    # 仓外库依赖之一——Linux 侧链 `-lz`，Windows clang-cl 侧用 ASYN_FUZZ_ZLIB_DIR 指路
    # （见文件头的跑法说明）
    "src/Net/WebSocket/PerMessageDeflate.cpp"
    # 出站响应解析器档：状态行/头部/chunked 三相位。头部规则与 HttpDate 已在上面对应位置列出
    "src/Net/Http/Client/HttpResponseParser.cpp"
    # JWT 档的本体：三段拆分与 base64url 解码、声明校验、JWK 的 n/e 进 OpenSSL 造公钥。
    # 它是本闭包第二份仓外依赖（libcrypto），Linux 侧 -lcrypto、Windows 侧按下面的开关指路
    "src/Net/Http/Auth/JwtVerifier.cpp"
    # ConfigValueText 与 LogEscaping 两档不需要新翻译单元：parse/serialize/configValueAs 与
    # escapeForLog 都是头文件内联的，仓外依赖只有 nlohmann（下面已为 HttpRequest 装好）。

    # 两档渲染器（RFC 7807 错误体、OTLP 正文）刻意不在这里：ProblemJson 拖进 HttpResponse 与
    # Base/Log、Core::CoroutinePool、Platform::MemoryMappedFile 整条链，实测链接期报 11 个未定义。

    # 本闭包按 /MT 编，链不上项目的 /MD 库，那条链只能整份重编；随机用例改落在
    # tests/Net/Http/TestProblemJson.cpp 与 tests/Net/Logging、tests/Net/Tracing 的渲染器测试里，
    # 那三处链完整库，两边平台、每轮 ctest 都跑。

    # AcmeClient 同理不在这份清单：它折 problem document 用的就是 LogEscaping 档吃的
    # Base::escapeForLog；那条整链要 HttpClient 与协程池，
    # 随机族用例落在 tests/Net/Acme/TestAcmeClient.cpp（链完整库）。
    "src/Net/Quic/Codec/QuicVariableLengthInteger.cpp"
    # QUIC 那三档解码器各自一个 .cpp：本闭包不链 Net 库，漏一项就在链接期报未定义符号
    # （Windows 侧编不出 libFuzzer 目标，所以这种缺失只有 Linux 跑批或 CI 会暴露）
    "src/Net/Quic/Codec/QuicPacketHeader.cpp"
    "src/Net/Quic/Codec/QuicFrame.cpp"
    "src/Net/Quic/Codec/QuicTransportParameters.cpp"
    "src/Net/Http/Message/HttpHeaderFieldStore.cpp"
    "src/Net/Http/Message/HttpRequest.cpp"
    # h1 请求档的本体：本闭包不链 Net 库，构造/parse/reset/request() 全在这里，漏一项链接期就报
    "src/Net/Http/Message/HttpParser.cpp"
    # 解析器直接调用的头部规则（逐跳头名判定等）也自成一份翻译单元
    "src/Net/Http/Message/HttpHeaderRules.cpp"
    # 请求对象的两条成员函数各自落在自己的文件里：cookies() 要 HttpCookie.cpp、multipartForm()
    # 要 MultipartForm.cpp，少哪一个都在链接期报未定义（清单本身就是按链接器的报错补齐的）
    "src/Net/Http/Message/HttpCookie.cpp"
    "src/Net/Http/Message/MultipartForm.cpp"
    "src/Net/Http/Message/HttpDate.cpp"
    "src/Base/Exception/Exception.cpp"
    # 三条异常链的抛出点与调用栈收成一份载荷基之后，Exception.cpp 只剩转调：载荷构造函数
    # 定义在自己的 .cpp 里，不列进来链接期就报 ExceptionPayload 未定义。
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
    # 编译器与语言档要探出来，不能写死：libstdc++ 的 <expected> 要求 __cpp_concepts >= 202002L，
    # clang 18 定在 201907L，任何 -std 下都看不见；clang 19 起才满足。
    # compiler-rt 的 fuzzer 运行时按 libstdc++ 编，-stdlib=libc++ 链接期会报 std::__cxx11 未定义。

    # 各台机器与 runner 上默认的 clang++ 是哪一档不可预知，就按候选挨个试到「编得过也链得上」为止。
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
        echo "运行时，而本仓各档解码器的接口两头都靠它们。libstdc++ 的 <expected> 要求 clang 报" >&2
        echo "__cpp_concepts >= 202002L，clang 18 及以下不满足：apt install clang-19 libfuzzer-19-dev，" >&2
        echo "或用 CXX=<路径> 指一份 clang 19 以上的编译器" >&2
        exit 1
    fi
    echo "编译器 ${compiler}，编译档 ${chosenFlags}（实测能编能链 std::expected 与 std::stop_source）"
    # 闭包里有一处仓外头文件：HttpRequest::jsonBody() 返回 Base::ConfigValue，
    # 它包着 nlohmann::json。目录从 ASYN_FUZZ_INCLUDE_DIRS 给（空格分隔），
    # Linux 装了 nlohmann-json3-dev 就不用给；先探一次，否则缺头文件在第十几个翻译单元才报错。
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
    # address 与 fuzzer 同时开：这批解码器的历史缺陷全是越界与释放后读，libFuzzer 自己看不出来。
    # 帧指针留着，崩溃栈才带得出行号。编译与链接用同一个 ${chosenFlags}：标准库两侧必须同一条，
    # 上面那次「能编也能链」的探针判的就是这一条。
    compileFlags=("${chosenFlags}" -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address -Isrc -Itests/Net "${nlohmannIncludes[@]}")
    # 两份仓外库依赖：permessage-deflate 档包着 zlib（-lz），JWT 档包着 OpenSSL 的 libcrypto
    # （-lcrypto，见 sources 处说明）。Ubuntu runner 两个都自带，CI 的 apt 步骤里也显式装了
    # zlib1g-dev 与 libssl-dev
    linkFlags=("${chosenFlags}" -fsanitize=fuzzer,address -lz -lcrypto)
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

# 以 '/' 开头的参数会被 MSYS 改写，因此每次调用都显式关掉路径转换；/MT 的理由见文件头。
# nlohmann/json.hpp 不在本仓（Base::ConfigValue 包着它），Windows 侧没有系统 include 可退，
# 用 ASYN_FUZZ_INCLUDE_DIRS 传 Conan 包目录（空格分隔），例如 .../p/include。
read -ra nlohmannIncludeDirs <<<"${ASYN_FUZZ_INCLUDE_DIRS:-}"
nlohmannIncludes=()
for includeDir in "${nlohmannIncludeDirs[@]}"; do
    nlohmannIncludes+=("/I${includeDir}")
done
# zlib 同样不在本仓（permessage-deflate 档包着它）：include 给编译器、lib/zlib.lib 给链接器
zlibDir="${ASYN_FUZZ_ZLIB_DIR:-}"
zlibFlags=()
zlibLinkFlags=()
if [[ -n "${zlibDir}" ]]; then
    zlibFlags=("/I${zlibDir}/include")
    zlibLinkFlags=("${zlibDir}/lib/zlib.lib")
fi
opensslDir="${ASYN_FUZZ_OPENSSL_DIR:-}"
opensslFlags=()
opensslLinkFlags=()
if [[ -n "${opensslDir}" ]]; then
    opensslFlags=("/I${opensslDir}/include")
    # JwtVerifier.cpp 只用 EVP/BN/param_build，全在 libcrypto 里，不需要 libssl
    opensslLinkFlags=("${opensslDir}/lib/libcrypto.lib")
fi
compileFlags=(/std:c++latest /O1 /MT /DNDEBUG /EHsc -fsanitize=fuzzer /Isrc /Itests/Net "${nlohmannIncludes[@]}" "${zlibFlags[@]}" "${opensslFlags[@]}")
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
MSYS_NO_PATHCONV=1 "${clangCl}" -fsanitize=fuzzer "/Fe${binary}" "${objects[@]}" "${zlibLinkFlags[@]}" "${opensslLinkFlags[@]}" || exit 1

MSYS_NO_PATHCONV=1 runAllTargets "${binary}" "${artifacts}" "$@"
exit $?
