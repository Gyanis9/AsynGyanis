@echo off
rem ============================================================================
rem Hot-path microbenchmark: run it, then compare against the recorded baseline;
rem a threshold violation exits non-zero.
rem See the microbench source header for usage and measurement rules; the only
rem argument is the build type, release by default.
rem ----------------------------------------------------------------------------
rem NOTE: keep this file ASCII-only; see the note in run-soak.bat for the reason.
rem ============================================================================
setlocal

set ASYN_BENCH_BUILD=%~1
if "%ASYN_BENCH_BUILD%"=="" set ASYN_BENCH_BUILD=release

set REPOSITORY_ROOT=%~dp0..
set BENCH_PATH=%REPOSITORY_ROOT%\build\%ASYN_BENCH_BUILD%\benchmarks\microbench\microbench.exe
if not exist "%BENCH_PATH%" (
    echo Cannot find %BENCH_PATH% - build the microbench target of %ASYN_BENCH_BUILD% first.
    exit /b 1
)

rem The result JSON is a transient run artifact, so it goes to %TEMP% as well: build\
rem only holds the debug/release build trees (see run-soak.bat for the same reasoning)
set RESULT_PATH=%TEMP%\asyn-microbench-%ASYN_BENCH_BUILD%.json
"%BENCH_PATH%" --json-out "%RESULT_PATH%"
if errorlevel 1 (
    echo Microbenchmark self-check failed: some case did not take its success path.
    exit /b 1
)

if /i not "%ASYN_BENCH_BUILD%"=="release" (
    echo.
    echo Non-release build, skipping the baseline comparison.
    exit /b 0
)

rem Within-run spread is only a few percent (each case already takes the best of 5
rem rounds), so the lower bound is tighter than the soak one
python "%REPOSITORY_ROOT%\benchmarks\check-baseline.py" ^
    --baseline "%REPOSITORY_ROOT%\benchmarks\microbench-baseline.json" ^
    --minimum-throughput-ratio 0.8 ^
    "%RESULT_PATH%"
exit /b %ERRORLEVEL%
