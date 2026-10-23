#!/usr/bin/env bash
# 打印格式门管理的全部跟踪源文件（NUL 分隔）；--check 校验输入面没被改窄。
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

# clang-format 支持的 C 家族扩展名；模板显式列出（它没有 .h 后缀）
format_patterns=(
    '*.h' '*.hh' '*.hpp' '*.hxx'
    '*.c' '*.cc' '*.cpp' '*.cxx'
    '*.inl' '*.ipp' '*.tpp'
    '*.cu' '*.cuh' '*.m' '*.mm'
    'cmake/AsynGyanisFeatures.h.in'
)

if [ "${1:-}" = "--check" ]; then
    # 输入面为空或少了代表性文件，说明 pathspec 被改窄了：
    # 格式门会从“全仓检查”悄悄退化成“什么都没查”。
    list="$(git ls-files -- "${format_patterns[@]}")"
    [ -n "$list" ] || { echo "格式清单为空：clang-format 没有任何输入" >&2; exit 2; }
    for required in src/Core/EventLoop/EventLoop.cpp tests/Core/EventLoop/TestEventLoop.cpp cmake/AsynGyanisFeatures.h.in; do
        printf '%s\n' "$list" | grep -qxF "$required" \
            || { echo "格式清单缺少 $required：输入面被改窄了" >&2; exit 2; }
    done
fi

git ls-files -z -- "${format_patterns[@]}"
