#!/usr/bin/env bash
# 一条命令跑完本仓库的门禁链，判据形状与 CI 一致：静态九道 + 构建 + 用例 + 示例矩阵。
# 门禁名单以 GATE_NAMES 为准：comment-budget、complexity、arch-trace、ledger-date、format、
# api-baseline、layering、platform-boundary、duplicates、build、ctest、samples。

# 用法（任意目录可调用：仓库根取自脚本位置；ctest --preset 必须在根上跑，脚本自己 cd 过去）：
#   bash scripts/run-gates.sh                                  # 全部（默认档），debug 预设
#   bash scripts/run-gates.sh --preset release

#   bash scripts/run-gates.sh --gate format --gate arch-trace  # 只跑点名的几道
#   bash scripts/run-gates.sh --format container               # 格式档强制走容器
#   bash scripts/run-gates.sh --help

# 输出形状：每道门在 stdout 打一行既有形状的判据（COMPLEXITY_GATE=OK、ARCH_TRACE_GATE=OK、
# API_GATE=OK、LAYERING_GATE=OK、PLATFORM_BOUNDARY_GATE=OK、DUPLICATE_CODE_GATE=OK…），
# 门的原始输出进 build/gate-logs/<门名>.log，判红的那道门把输出整片贴出来。

# 最后一行给总判据与机读 JSON（build/gate-logs/run-gates.json）：每道门的命令、退出码、耗时、
# 日志位置，还有总耗时与结论。JSON 按 UTF-8 写（结论文案是中文），Windows 上读它要显式给
# encoding='utf-8'——本地 locale 是 cp936 时 json.load(open(path)) 崩在读那一行。

# 阈值不在这里抄第二份：复杂度地板、覆盖率下限、格式清单数量下限只有工作流与门禁脚本一个出处，
# 本脚本只按顺序调用并如实汇总。

# 格式档的 clang-format 版本钉在 .github/workflows/linux-ci.yml 的 pip 安装行，本脚本现读那一行；
# 本机 PATH 上的（或 ASYN_CLANG_FORMAT 指的）版本对得上就地用，对不上就换容器里那份：
# 版本不同折行结果就变，那种红与改动无关。

# 容器路线前提：docker 在 PATH 上、目标容器在跑、容器里有 /root/fmt-venv/bin/clang-format；
# 调用 bash scripts/run-gates.sh --format container --format-container <容器名>。

# 机制：源文件连同 .clang-format 与清单打成 tar 从 stdin 送进容器临时目录解包，
# 在解包目录里跑钉版 clang-format --style=file --dry-run -Werror，用完删临时目录。

# 本链镜像 CI 的格式作业加构建作业那几道静态与功能门；覆盖率、TSan、模糊、交付物、供应链、
# 微基准各有作业与前置（服务、容器、插桩构建），不在这条命令里，别把它读成「跑完就等于 CI 全绿」。

# 退出码：0 全绿；1 某道门判红（构建红时后面没有产物可跑，链在此停住）；
# 2 前置缺件（python、clang-format、cmake、容器、构建树配置不下来——「没跑成」不许读成「没问题」）。

# Windows 侧先 call vcvars64（或在 Developer Command Prompt 里跑）再进来，否则 cl 读不到
# INCLUDE/LIB，构建那一档会红在编译参数上而不是代码上。
set -u -o pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 2
# JSON 里的根路径写成本机可用的形状：Git Bash 的 /g/… 只有本环境认，而读这份 JSON 的原生工具
# 拿到它就是一条不存在的路径；pwd -W 在 Linux 上不可用，退回 pwd 就是同一个值
JSON_ROOT="$(pwd -W 2>/dev/null || pwd)"

LOG_DIR="build/gate-logs"
JSON_PATH="$LOG_DIR/run-gates.json"
CONTAINER_FORMAT_BIN="/root/fmt-venv/bin/clang-format"

preset="debug"
formatMode="auto"
formatContainer="ubuntu24"
selectedGates=""

PASSED_COUNT=0
RUN_COUNT=0
FAILED_GATES=""
GATE_ENTRIES=""
TOTAL_ELAPSED=0
START_EPOCH="$(date +%s)"
STARTED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

# --gate 没给就是全跑；给了就只跑点名的几道
GATE_NAMES="comment-budget complexity arch-trace ledger-date format api-baseline layering platform-boundary duplicates build ctest samples"

usage()
{
    printf '用法：bash scripts/run-gates.sh [--preset debug|release] [--gate NAME]... [--format auto|local|container] [--format-container 容器名]\n'
    printf '可选门名：%s\n' "$GATE_NAMES"
    printf '前置缺件退 2，判红退 1，全绿退 0\n'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --preset)
            [ $# -ge 2 ] || { echo "--preset 后面要跟档名" >&2; exit 2; }
            preset="$2"
            shift 2
            ;;
        --gate)
            [ $# -ge 2 ] || { echo "--gate 后面要跟门名" >&2; exit 2; }
            case " $GATE_NAMES " in *" $2 "*) ;; *) printf '不认识的门名：%s（可选：%s）\n' "$2" "$GATE_NAMES" >&2; exit 2 ;; esac
            selectedGates="$selectedGates $2"
            shift 2
            ;;
        --format)
            [ $# -ge 2 ] || { echo "--format 后面要跟 auto|local|container" >&2; exit 2; }
            case "$2" in auto|local|container) ;; *) echo "--format 只认 auto|local|container" >&2; exit 2 ;; esac
            formatMode="$2"
            shift 2
            ;;
        --format-container)
            [ $# -ge 2 ] || { echo "--format-container 后面要跟容器名" >&2; exit 2; }
            formatContainer="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf '不认识的参数：%s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

want()
{
    [ -z "$selectedGates" ] && return 0
    case " $selectedGates " in *" $1 "*) return 0 ;; esac
    return 1
}

jsonString()
{
    # 反斜杠与双引号要按 JSON 转义：Windows 的绝对路径里全是反斜杠，不转义产出的不是合法 JSON
    printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}

write_json()
{
    local result="$1" exitCode="$2" note="$3" nowEpoch
    nowEpoch="$(date +%s)"
    TOTAL_ELAPSED=$((nowEpoch - START_EPOCH))
    mkdir -p "$LOG_DIR" || return 0
    {
        printf '{\n'
        printf '  "repositoryRoot": "%s",\n' "$(jsonString "$JSON_ROOT")"
        printf '  "preset": "%s",\n' "$(jsonString "$preset")"
        printf '  "startedAt": "%s",\n' "$(jsonString "$STARTED_AT")"
        printf '  "elapsedSeconds": %s,\n' "$TOTAL_ELAPSED"
        printf '  "result": "%s",\n' "$(jsonString "$result")"
        printf '  "exitCode": %s,\n' "$exitCode"
        printf '  "note": "%s",\n' "$(jsonString "$note")"
        printf '  "gates": [\n'
        printf '%s\n' "$GATE_ENTRIES"
        printf '  ]\n'
        printf '}\n'
    } >"$JSON_PATH"
}

finish()
{
    local exitCode="$1" result="$2" note="$3"
    write_json "$result" "$exitCode" "$note"
    printf 'GATES=%s passed=%s/%s elapsed=%ss failed="%s" note="%s" json=%s\n' \
        "$result" "$PASSED_COUNT" "$RUN_COUNT" "$TOTAL_ELAPSED" "${FAILED_GATES# }" "$note" "$JSON_PATH"
    exit "$exitCode"
}

prereq_fail()
{
    printf '前置缺件：%s\n' "$1" >&2
    finish 2 PREREQ "前置缺件：$1"
}

# 执行者先落地：门禁脚本与示例矩阵都靠它，Windows 上 python3 常是商店占位符（跑不出东西、退 49）
PYTHON=""
for candidate in python3 python py; do
    if command -v "$candidate" >/dev/null 2>&1 && "$candidate" -c 'import sys' >/dev/null 2>&1; then
        PYTHON="$candidate"
        break
    fi
done
[ -n "$PYTHON" ] || prereq_fail "找不到可用的 python（python3、python、py 都不能跑）：静态门与示例矩阵没有执行者"

mkdir -p "$LOG_DIR" || prereq_fail "建不出日志目录 $LOG_DIR"

run_gate()
{
    local name="$1" verdictKey="$2" commandText="$3"
    shift 3
    local logPath="$LOG_DIR/$name.log" startTs endTs rc elapsed verdict entry
    startTs="$(date +%s)"
    "$@" >"$logPath" 2>&1
    rc=$?
    endTs="$(date +%s)"
    elapsed=$((endTs - startTs))
    RUN_COUNT=$((RUN_COUNT + 1))
    if [ "$rc" -eq 0 ]; then
        verdict=OK
        PASSED_COUNT=$((PASSED_COUNT + 1))
    else
        verdict=FAIL
        FAILED_GATES="$FAILED_GATES $name"
    fi
    printf '%s=%s rc=%s elapsed=%ss log=%s\n' "$verdictKey" "$verdict" "$rc" "$elapsed" "$logPath"
    if [ "$verdict" = FAIL ]; then
        # 判红时把那条门自己的输出贴出来：只留一句结论的话，读的人还得再去开日志
        printf -- '----- %s 的输出 -----\n' "$name"
        cat "$logPath"
        printf -- '----- %s 的输出结束 -----\n' "$name"
    fi
    entry="$(printf '    {"name": "%s", "command": "%s", "verdict": "%s", "exitCode": %s, "elapsedSeconds": %s, "log": "%s"}' \
        "$(jsonString "$name")" "$(jsonString "$commandText")" "$verdict" "$rc" "$elapsed" "$(jsonString "$logPath")")"
    if [ -n "$GATE_ENTRIES" ]; then
        GATE_ENTRIES="$GATE_ENTRIES,"$'\n'"$entry"
    else
        GATE_ENTRIES="$entry"
    fi
    return "$rc"
}

check_format_local()
{
    # 清单由 list-format-files.sh 统一生成；xargs 的 123 表示文件有差异，如实回传
    xargs -0 "$1" --style=file --dry-run -Werror < "$2"
}

check_format_container()
{
    local container="$1" workDir="$2"
    # 容器内先进解包目录：清单里的路径是按仓库根相对的，.clang-format 就在那一层的顶上，
    # --style=file 的向上查找因此和在本机跑时同解
    MSYS_NO_PATHCONV=1 docker exec "$container" sh -c \
        "cd '$workDir' && xargs -0 '$CONTAINER_FORMAT_BIN' --style=file --dry-run -Werror < '$workDir/$LOG_DIR/format-list.z'"
}

run_format_gate()
{
    local pinned bin reported fileCount listFile tarInput workDir rc
    pinned="$(sed -n 's/.*clang-format==\([0-9][0-9.]*\).*/\1/p' .github/workflows/linux-ci.yml | head -n 1)"
    [ -n "$pinned" ] || prereq_fail "读不到 .github/workflows/linux-ci.yml 里钉住的 clang-format 版本，格式档没有可比对的裁判"

    listFile="$LOG_DIR/format-list.z"
    bash scripts/list-format-files.sh --check > "$listFile" || prereq_fail "格式清单生成失败：list-format-files.sh --check 退非零"
    fileCount="$(grep -z -c '' "$listFile")"
    # 清单为空等于「什么都没查」：CI 里那条数量下限判的是同一个形状，这里不抄它的数字，只判有没有
    [ "$fileCount" -gt 0 ] || prereq_fail "格式清单为空，格式检查不会查到任何东西"

    if [ "$formatMode" != container ]; then
        bin="${ASYN_CLANG_FORMAT:-$(command -v clang-format || true)}"
        if [ -n "$bin" ]; then
            reported="$("$bin" --version 2>/dev/null | sed -n 's/.*version \([0-9][0-9.]*\).*/\1/p' | head -n 1)"
            if [ "$reported" = "$pinned" ]; then
                printf '格式档裁判：本机 %s（版本 %s，与 CI 钉住的同一版），参与检查 %s 个跟踪源文件\n' "$bin" "$reported" "$fileCount"
                run_gate format FORMAT_GATE "clang-format --style=file --dry-run -Werror 吃 list-format-files.sh 的源文件清单" \
                    check_format_local "$bin" "$listFile"
                return $?
            fi
            # 点名要本机那份时不许偷偷换裁判：读的人以为判的是 CI 那一版，实际判的是另一件工具，
            # 那种「绿」与换版本造成的折行差异一样会骗人
            [ "$formatMode" != local ] || prereq_fail "--format local 指定的那件报的是 ${reported:-取不到版本}（$bin），与 CI 钉住的 $pinned 不同；装同版：pip install clang-format==$pinned"
            printf '格式档裁判：本机 %s 报的是 %s，与 CI 钉住的 %s 不同，换容器那份\n' "$bin" "${reported:-取不到版本}" "$pinned"
        elif [ "$formatMode" = local ]; then
            prereq_fail "--format local 要本机 clang-format，但 PATH 上（与 ASYN_CLANG_FORMAT 里）都没有；装同版：pip install clang-format==$pinned"
        else
            printf '格式档裁判：本机没有 clang-format，换容器那份\n'
        fi
    fi

    command -v docker >/dev/null 2>&1 || prereq_fail "格式档需要 clang-format $pinned：本机没有可用的（或版本不符），而 docker 不在 PATH 上，容器路线也走不通（本机装一件：pip install clang-format==$pinned）"
    local running
    running="$(docker inspect -f '{{.State.Running}}' "$formatContainer" 2>/dev/null || echo 取不到)"
    [ "$running" = "true" ] || prereq_fail "容器 $formatContainer 没在跑（读到 $running），格式档没有裁判：先起容器，或本机装 clang-format==$pinned"
    reported="$(MSYS_NO_PATHCONV=1 docker exec "$formatContainer" "$CONTAINER_FORMAT_BIN" --version 2>/dev/null | sed -n 's/.*version \([0-9][0-9.]*\).*/\1/p' | head -n 1)"
    [ "$reported" = "$pinned" ] || prereq_fail "容器 $formatContainer 里的 $CONTAINER_FORMAT_BIN 报的是 ${reported:-那件工具不存在}，与 CI 钉住的 $pinned 不同"

    tarInput="$LOG_DIR/format-tar-input.z"
    cp "$listFile" "$tarInput" || prereq_fail "准备 tar 清单失败"
    # .clang-format 与清单本身一起进包：配置不在包里，--style=file 会静默退回默认档并把整片重排
    printf '%s\0%s\0' '.clang-format' "$listFile" >>"$tarInput"
    workDir="/tmp/asyn-gate-format.$$"

    printf '格式档裁判：容器 %s 里的 %s（版本 %s），参与检查 %s 个跟踪源文件\n' "$formatContainer" "$CONTAINER_FORMAT_BIN" "$reported" "$fileCount"
    tar -C "$REPO_ROOT" --null -T "$tarInput" -cf - \
        | MSYS_NO_PATHCONV=1 docker exec -i "$formatContainer" sh -c 'rm -rf "$1" && mkdir -p "$1" && tar -xf - -C "$1"' sh "$workDir" \
        || prereq_fail "把源文件送进容器 $formatContainer 这一步断了，格式档这一轮没有跑成"

    run_gate format FORMAT_GATE "容器 $formatContainer 的 clang-format --style=file --dry-run -Werror（源文件经 tar 送进 $workDir）" \
        check_format_container "$formatContainer" "$workDir"
    rc=$?
    # 临时目录是裁判的现场，不是产物：无论红绿都删，别让下一轮的解包读到上一轮的内容
    MSYS_NO_PATHCONV=1 docker exec "$formatContainer" rm -rf "$workDir" >/dev/null 2>&1 || true
    return "$rc"
}

configure_tree()
{
    local raw binaryDir
    command -v cmake >/dev/null 2>&1 || prereq_fail "cmake 不在 PATH 上：构建与用例都没有执行者"
    # binaryDir 从预设现读，不在脚本里另写一份目录名（改了预设不必回来改这里）
    raw="$("$PYTHON" -c 'import json,sys;d=json.load(open("CMakePresets.json"));print(next(p["binaryDir"] for p in d["configurePresets"] if p["name"]==sys.argv[1]))' "$preset" 2>/dev/null)"
    [ -n "$raw" ] || prereq_fail "CMakePresets.json 里没有名为 $preset 的配置预设，构建与用例没有目录可去"
    binaryDir="$(printf '%s' "$raw" | sed "s|\${sourceDir}|$REPO_ROOT|g")"
    # 落在仓库内就换成相对路径：msys 的 /g/… 那种绝对写法只有本环境的工具认，
    # 原生 python（示例矩阵的执行者）拿到它就是一句「找不到目录」
    binaryDir="${binaryDir#"$REPO_ROOT"/}"
    if [ ! -f "$binaryDir/CMakeCache.txt" ]; then
        # 配置是前置而不是判据：配不下来就是拿不到裁判，不该记成「构建判红」
        printf '构建树 %s 还没配置，先跑 cmake --preset %s\n' "$binaryDir" "$preset"
        cmake --preset "$preset" >"$LOG_DIR/configure.log" 2>&1 || prereq_fail "cmake --preset $preset 配置失败，细节见 $LOG_DIR/configure.log"
    fi
    BINARY_DIR="$binaryDir"
}

buildGate()
{
    # -- -k 0 与 CI 的构建步骤同形：一次编完把所有告警与错误收齐，逐个文件报错要修一轮等一轮
    cmake --build --preset "$preset" -- -k 0
}

testGate()
{
    # 并行度取现测核数：写死在预设里就把「机器多快」编进了判据，而 -j 只影响用时不影响结论
    # --no-tests=error：一条都没选中判红而不是静默绿——「什么都没跑」与「全跑绿」外观同形
    local jobs
    jobs="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
    ctest --preset "$preset" -j "$jobs" --no-tests=error
}

samplesGate()
{
    # 时限用 run_samples.py 自己的默认值：CI 里那个平台专属的秒数写在两个工作流里，这里不抄第三份
    "$PYTHON" scripts/run_samples.py --build "$BINARY_DIR"
}

if want build || want ctest || want samples; then
    configure_tree
fi

if want layering || want platform-boundary || want duplicates; then
    run_gate arch-gate-selftest ARCH_GATE_SELFTEST "$PYTHON scripts/test-architecture-gates.py" \
        "$PYTHON" scripts/test-architecture-gates.py
fi

if want complexity; then
    run_gate complexity COMPLEXITY_GATE "$PYTHON scripts/check-complexity-baseline.py" \
        "$PYTHON" scripts/check-complexity-baseline.py
fi

if want arch-trace; then
    run_gate arch-trace ARCH_TRACE_GATE "$PYTHON scripts/check-arch-traceability.py" \
        "$PYTHON" scripts/check-arch-traceability.py
fi

if want ledger-date; then
    run_gate ledger-date LEDGER_DATE_GATE "$PYTHON scripts/check-ledger-dates.py" \
        "$PYTHON" scripts/check-ledger-dates.py
fi

if want comment-budget; then
    run_gate comment-budget COMMENT_BUDGET_GATE "$PYTHON scripts/check-comment-budget.py" \
        "$PYTHON" scripts/check-comment-budget.py
fi

if want format; then
    run_format_gate
fi

if want api-baseline; then
    run_gate api-baseline API_GATE "$PYTHON scripts/check-api-baseline.py" \
        "$PYTHON" scripts/check-api-baseline.py
fi

if want layering; then
    run_gate layering LAYERING_GATE "$PYTHON scripts/check-module-layering.py" \
        "$PYTHON" scripts/check-module-layering.py
fi

if want platform-boundary; then
    run_gate platform-boundary PLATFORM_BOUNDARY_GATE "$PYTHON scripts/check-platform-boundary.py" \
        "$PYTHON" scripts/check-platform-boundary.py
fi

if want duplicates; then
    run_gate duplicates DUPLICATE_CODE_GATE "$PYTHON scripts/check-duplicate-code.py" \
        "$PYTHON" scripts/check-duplicate-code.py
fi

if want build; then
    if run_gate build BUILD_GATE "cmake --build --preset $preset -- -k 0" buildGate; then
        :
    elif want ctest || want samples; then
        # 构建红时后面的用例与示例没有产物可跑，再跑只会报一堆与真因无关的红；如实说明停在哪
        finish 1 FAIL "构建判红，用例与示例没有产物可跑，链在此停住"
    fi
fi

if want ctest; then
    run_gate ctest CTEST_GATE "ctest --preset $preset -j <现测核数> --no-tests=error" testGate
fi

if want samples; then
    run_gate samples SAMPLES_GATE "$PYTHON scripts/run_samples.py --build $BINARY_DIR" samplesGate
fi

if [ -n "$FAILED_GATES" ]; then
    finish 1 FAIL "判红的门：${FAILED_GATES# }"
fi
finish 0 OK "全部点名的门都过了"
