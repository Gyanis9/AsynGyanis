@echo off
rem ============================================================================
rem Out-of-process soak: start the sample server, run benchmarks/soak.py, finish.
rem Usage: benchmarks\run-soak.bat <port> [workerThreads] [extra soak.py args...]
rem Requires: the echo_server target already built for the chosen build type.
rem Build type: debug by default (build\debug); set ASYN_SOAK_BUILD=release to use build\release.
rem       Debug carries AddressSanitizer, so its throughput only compares with other
rem       Debug runs and is not a performance ceiling; comparing against
rem       benchmarks/baseline.json must use release, where that baseline was measured.
rem Gate usage: ... run-soak.bat 18080 4 --json-out build\soak.json
rem       then: python benchmarks\check-baseline.py build\soak.json
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

set REPOSITORY_ROOT=%~dp0..
set SERVER_PATH=%REPOSITORY_ROOT%\build\%ASYN_SOAK_BUILD%\samples\echo_server.exe
if not exist "%SERVER_PATH%" (
    echo Cannot find %SERVER_PATH% - build the echo_server target of %ASYN_SOAK_BUILD% first.
    exit /b 1
)

rem The server's output must land in a file: a minimized console window is gone the
rem moment the process is killed, and then a crash during the soak leaves no evidence
set SERVER_LOG=%TEMP%\asyn-soak-server-%SERVER_PORT%.log
echo Starting %SERVER_PATH% --port %SERVER_PORT% --threads %SERVER_THREADS%
echo Server log: %SERVER_LOG%
start "AsynGyanis soak server" /MIN cmd /c ""%SERVER_PATH%" --port %SERVER_PORT% --threads %SERVER_THREADS% > "%SERVER_LOG%" 2>&1"

rem Wait for the server process: up to 30 seconds, one try per second
set SERVER_PID=
for /l %%i in (1,1,30) do (
    if not defined SERVER_PID (
        for /f "tokens=2 delims=," %%p in ('tasklist /FI "IMAGENAME eq echo_server.exe" /FO CSV /NH 2^>nul') do set SERVER_PID=%%~p
        if not defined SERVER_PID timeout /t 1 /nobreak >nul
    )
)
if not defined SERVER_PID (
    echo Server did not start within 30 seconds, giving up. See %SERVER_LOG%
    exit /b 1
)
echo Server pid %SERVER_PID%

python "%REPOSITORY_ROOT%\benchmarks\soak.py" --port %SERVER_PORT% --pid %SERVER_PID% %3 %4 %5 %6 %7 %8 %9
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
