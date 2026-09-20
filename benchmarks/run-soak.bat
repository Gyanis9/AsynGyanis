@echo off
rem ============================================================================
rem Out-of-process soak: start the sample server, run benchmarks/soak.py, finish.
rem Usage: benchmarks\run-soak.bat <port> [workerThreads] [extra soak.py args...]
rem Requires: the echo_server target already built for the chosen build type.
rem Build type: debug by default (build\debug); set ASYN_SOAK_BUILD=release to use build\release.
rem       Debug carries AddressSanitizer, so its throughput only compares with other
rem       Debug runs and is not a performance ceiling; comparing against
rem       benchmarks/baseline.json must use release, where that baseline was measured.
rem Gate usage (baseline.json has three groups: http1-*, http2-h2c-pipeline1, http2-h2c-pipeline32):
rem   1) ... run-soak.bat 18080 4 --json-out build\soak.json
rem   2) start the server again WITH --h2c, then run the h2c probe TWICE: soak_h2c.py
rem      names its result after --pipeline, so a single run can only ever fill one entry
rem      (its default is 32, which leaves pipeline1 permanently "missing").
rem      python benchmarks\soak_h2c.py --port 18080 --pipeline 1  --json-out build\soak-h2c-p1.json
rem      python benchmarks\soak_h2c.py --port 18080 --pipeline 32 --json-out build\soak-h2c.json
rem   3) python benchmarks\check-baseline.py build\soak.json build\soak-h2c-p1.json build\soak-h2c.json
rem NOTE: a baseline entry that none of the fed files contains is reported as missing and
rem       the gate exits 1 - that is a setup mistake, not a regression. The three-file
rem       sequence above was re-run on 2026-09-20 and reports 0 violations.
rem NOTE: the HTTP/1.1 probe must face a server WITHOUT --h2c: an h2c port treats the
rem       connection as HTTP/2 prior knowledge and answers a plain request with GOAWAY.
rem ----------------------------------------------------------------------------
rem NOTE: keep this file ASCII-only. cmd parses batch files using the console OEM
rem       code page (936 on zh-CN Windows), so UTF-8 comments are split mid-line and
rem       cmd then tries to run the fragments as commands. Chinese prose belongs in
rem       README/CHANGELOG, not in a .bat.
rem ============================================================================
setlocal

set SERVER_PORT=%~1
if "%SERVER_PORT%"=="" set SERVER_PORT=18080
set SERVER_THREADS=%~2
if "%SERVER_THREADS%"=="" set SERVER_THREADS=4
if "%ASYN_SOAK_BUILD%"=="" set ASYN_SOAK_BUILD=debug

rem Rate-limit mode: start the server with a small token bucket so soak.py can prove
rem that 429 + retry-after actually happen. Load stages are skipped in that mode: with a
rem 2 req/s bucket they would all be rejected by the limiter itself.
set RATE_LIMIT_ARGS=
set SOAK_EXTRA_ARGS=
if "%ASYN_SOAK_RATE_LIMIT%"=="1" (
    rem Inside a parenthesized block cmd expands %VAR% at parse time, so only variables set
    rem before the block (TEMP, SERVER_PORT) may be referenced here; the two variables set
    rem below are read after the block, where expansion is correct.
    > "%TEMP%\asyn-soak-ratelimit-%SERVER_PORT%.yaml" echo server:
    >> "%TEMP%\asyn-soak-ratelimit-%SERVER_PORT%.yaml" echo   rate_limit:
    >> "%TEMP%\asyn-soak-ratelimit-%SERVER_PORT%.yaml" echo     requests_per_second: 2
    >> "%TEMP%\asyn-soak-ratelimit-%SERVER_PORT%.yaml" echo     burst_capacity: 2
    set RATE_LIMIT_ARGS=--config "%TEMP%\asyn-soak-ratelimit-%SERVER_PORT%.yaml"
    set SOAK_EXTRA_ARGS=--skip-load-stages --rate-limit-burst 20
    echo Rate limit mode: %TEMP%\asyn-soak-ratelimit-%SERVER_PORT%.yaml (2 requests/s, burst 2)
)

set REPOSITORY_ROOT=%~dp0..
set SERVER_PATH=%REPOSITORY_ROOT%\build\%ASYN_SOAK_BUILD%\samples\echo_server.exe
if not exist "%SERVER_PATH%" (
    echo Cannot find %SERVER_PATH% - build the echo_server target of %ASYN_SOAK_BUILD% first.
    exit /b 1
)

rem The server's output must land in a file: a minimized console window is gone the
rem moment the process is killed, and then a crash during the soak leaves no evidence
set SERVER_LOG=%TEMP%\asyn-soak-server-%SERVER_PORT%.log
echo Starting %SERVER_PATH% --port %SERVER_PORT% --threads %SERVER_THREADS% %RATE_LIMIT_ARGS%
echo Server log: %SERVER_LOG%
start "AsynGyanis soak server" /MIN cmd /c ""%SERVER_PATH%" --port %SERVER_PORT% --threads %SERVER_THREADS% %RATE_LIMIT_ARGS% > "%SERVER_LOG%" 2>&1"

rem Wait for the server process: up to 30 seconds, one try per second. Plain tasklist
rem is not enough: it lists every echo_server.exe, so a leftover instance (or a second
rem soak on another port) would hand us a pid that is not ours - soak.py would then
rem measure the wrong process, and the taskkill at the end would shoot someone else's
rem server. Match on the command line instead.
set SERVER_PID=
for /l %%i in (1,1,30) do call :FindServerPid
if not defined SERVER_PID (
    echo Server did not start within 30 seconds, giving up. See %SERVER_LOG%
    exit /b 1
)
echo Server pid %SERVER_PID%

python "%REPOSITORY_ROOT%\benchmarks\soak.py" --port %SERVER_PORT% --pid %SERVER_PID% %SOAK_EXTRA_ARGS% %3 %4 %5 %6 %7 %8 %9
set SOAK_EXIT_CODE=%ERRORLEVEL%

rem Failures are usually the server's fault: show what it printed before it went away
if not "%SOAK_EXIT_CODE%"=="0" (
    echo ----------------------------------------------------------------------------
    echo Soak failed, server log follows: %SERVER_LOG%
    echo ----------------------------------------------------------------------------
    type "%SERVER_LOG%"
)

rem The soak script ends by shutting the server down itself; kill the process directly
rem here (the graceful drain path is demonstrated separately by samples/main.cpp)
taskkill /F /PID %SERVER_PID% >nul 2>&1

echo Soak finished, exit code %SOAK_EXIT_CODE% (0 means zero protocol and payload failures)
exit /b %SOAK_EXIT_CODE%

rem Helper for the wait loop above: set SERVER_PID to the pid of the echo_server.exe
rem that was started with our port. Stay ASCII-only and keep parentheses out of the
rem PowerShell command: cmd counts bare parens when parsing the for /f set. Keep the
rem test down to one -match as well: PowerShell gives -and and -or the same precedence
rem and evaluates them left to right, so "A -and B -or A -and C" is not "A -and (B -or C)".
rem The word boundary accepts both "--port 8080 " and a port at the end of the line.
:FindServerPid
if defined SERVER_PID exit /b 0
for /f "usebackq tokens=*" %%p in (`powershell -NoProfile -NonInteractive -Command "Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'echo_server.exe' -and $_.CommandLine -match '--port\s+%SERVER_PORT%\b' } | Select-Object -First 1 -ExpandProperty ProcessId" 2^>nul`) do set SERVER_PID=%%p
if not defined SERVER_PID ping -n 2 127.0.0.1 >nul
exit /b 0
