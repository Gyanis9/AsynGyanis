@echo off
rem 热路径微基准：跑起来后与基准文件逐项比对，超阈值即非零退出。
rem 用法与口径见微基准源码头部；参数只接受构建类型一个值，默认发布构建。
rem ============================================================================
setlocal

set ASYN_BENCH_BUILD=%~1
if "%ASYN_BENCH_BUILD%"=="" set ASYN_BENCH_BUILD=release

set REPOSITORY_ROOT=%~dp0..
set BENCH_PATH=%REPOSITORY_ROOT%\build\%ASYN_BENCH_BUILD%\benchmarks\microbench\microbench.exe
if not exist "%BENCH_PATH%" (
    echo 找不到 %BENCH_PATH%，请先构建 %ASYN_BENCH_BUILD% 目标的 microbench。
    exit /b 1
)

set RESULT_PATH=%REPOSITORY_ROOT%\build\microbench-%ASYN_BENCH_BUILD%.json
"%BENCH_PATH%" --json-out "%RESULT_PATH%"
if errorlevel 1 (
    echo 微基准自检失败：有用例没有走到成功路径，先看上面的输出。
    exit /b 1
)

if /i not "%ASYN_BENCH_BUILD%"=="release" (
    echo.
    echo 非 release 构建，跳过基线比对。
    exit /b 0
)

rem 微基准的轮内离散只有几个百分点（用例内已做 5 轮取最快），下限比压测那份严
python "%REPOSITORY_ROOT%\benchmarks\check-baseline.py" ^
    --baseline "%REPOSITORY_ROOT%\benchmarks\microbench-baseline.json" ^
    --minimum-throughput-ratio 0.8 ^
    "%RESULT_PATH%"
exit /b %ERRORLEVEL%
