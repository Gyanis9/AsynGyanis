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
rem Remove the previous run's artifact first: the JSON path is reused, so a run that dies
rem before writing it would otherwise be graded against last run's numbers and pass.
del "%RESULT_PATH%" >nul 2>nul
"%BENCH_PATH%" --json-out "%RESULT_PATH%"
rem Compare against zero rather than "if errorlevel 1": cmd evaluates that test as a SIGNED
rem comparison, so a crash exit code (0xC0000005 shows up as -1073741819) or ninja's 0xFFFFFFFF
rem reads as "not at least 1" and the guard lets the run through as a success.
if not "%ERRORLEVEL%"=="0" (
    echo Microbenchmark run failed with exit code %ERRORLEVEL% - self-check or crash, not a regression verdict.
    exit /b 1
)
if not exist "%RESULT_PATH%" (
    echo Microbenchmark exited without writing %RESULT_PATH% - refusing to grade a stale or missing result.
    exit /b 1
)

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
    "%RESULT_PATH%"
exit /b %ERRORLEVEL%
