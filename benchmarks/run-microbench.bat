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

rem Three runs, graded by median. Two reasons, both observed rather than theoretical:
rem the recorded baseline numbers are medians of three runs (microbench-baseline.json says
rem so), so grading one run against them compares two different quantities; and the single
rem scheduling-bound case here - eventloop-remote-post-roundtrip, a cross-thread wakeup -
rem moves between process modes (today: 59k, 81k, 88k, 81k throughput/s on one binary),
rem because where the loop thread lands is per-process luck. Median of three keeps the
rem verdict on order-of-magnitude regressions while stopping the gate from firing on placement noise.
rem The result JSONs are transient run artifacts, so they go to %TEMP% as well: build\
rem only holds the debug/release build trees (see run-soak.bat for the same reasoning).
set RESULT_1=%TEMP%\asyn-microbench-%ASYN_BENCH_BUILD%-run1.json
set RESULT_2=%TEMP%\asyn-microbench-%ASYN_BENCH_BUILD%-run2.json
set RESULT_3=%TEMP%\asyn-microbench-%ASYN_BENCH_BUILD%-run3.json

call :runcase "%RESULT_1%"
if not "%ERRORLEVEL%"=="0" exit /b 1
call :runcase "%RESULT_2%"
if not "%ERRORLEVEL%"=="0" exit /b 1
call :runcase "%RESULT_3%"
if not "%ERRORLEVEL%"=="0" exit /b 1

if /i not "%ASYN_BENCH_BUILD%"=="release" (
    echo.
    echo Non-release build, skipping the baseline comparison.
    exit /b 0
)

rem Within-run spread is a few percent (each case keeps the best of five rounds), so the
rem lower bound here is tighter than the soak one. That claim used to be false for one
rem case: header-first-value kept the returned optional<string> inside the timed body, so
rem every round took and gave back a heap block, and that size class's luck moved the
rem reading between ~320 ns and ~540 ns on the *same* binary depending on unrelated code
rem layout - neighbouring cases stayed within 1% the whole time, so it was never the lookup
rem getting slower. Hoisting that optional out brought three consecutive gated runs back to
rem 320-352 ns. Keep the habit anyway: when one case looks off, compare it against
rem header-refill-only (same allocations, no query) before believing a regression, and
rem remember that all cases share one process, so an earlier case leaves heap state for a
rem later one - worth a few percent, which is why no case here should be read as exact.
python "%REPOSITORY_ROOT%\benchmarks\check-baseline.py" ^
    --baseline "%REPOSITORY_ROOT%\benchmarks\microbench-baseline.json" ^
    --minimum-throughput-ratio 0.8 ^
    "%RESULT_1%" "%RESULT_2%" "%RESULT_3%"
exit /b %ERRORLEVEL%

rem ----------------------------------------------------------------------------
rem One measurement pass. ERRORLEVEL is captured into a variable before the tests
rem because inside a parenthesised block it would expand at parse time, and the
rem comparison is against "0" rather than "if errorlevel 1" because cmd evaluates
rem that test as SIGNED, so a crash exit code (0xC0000005 shows up as -1073741819)
rem or ninja's 0xFFFFFFFF would read as "not at least 1" and pass as a success.
:runcase
del %1 >nul 2>nul
"%BENCH_PATH%" --json-out %1
set RUN_RESULT=%ERRORLEVEL%
if not "%RUN_RESULT%"=="0" (
    echo Microbenchmark run failed with exit code %RUN_RESULT% - self-check or crash, not a regression verdict.
    exit /b 1
)
if not exist %1 (
    echo Microbenchmark exited without writing %~1 - refusing to grade a stale or missing result.
    exit /b 1
)
exit /b 0
