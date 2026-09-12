@echo off
rem ============================================================================
rem 进程外压测：启动示例服务器 → 跑 benchmarks/soak.py → 收工。
rem 用法：benchmarks\run-soak.bat <端口> [工作线程数] [压测脚本附加参数...]
rem 前提：先构建过 Debug（build\debug\samples\echo_server.exe 存在）。
rem 注意：Debug 构建带 AddressSanitizer，测出的吞吐只用于同类构建的回归对比，
rem       不是框架的性能上限；绝对数字要用 Release 构建另测。
rem ============================================================================
setlocal

set SERVER_PORT=%~1
if "%SERVER_PORT%"=="" set SERVER_PORT=18080
set SERVER_THREADS=%~2
if "%SERVER_THREADS%"=="" set SERVER_THREADS=4

set REPOSITORY_ROOT=%~dp0..
set SERVER_PATH=%REPOSITORY_ROOT%\build\debug\samples\echo_server.exe
if not exist "%SERVER_PATH%" (
    echo 找不到 %SERVER_PATH%，请先构建 Debug 目标 echo_server。
    exit /b 1
)

echo 启动 %SERVER_PATH% --port %SERVER_PORT% --threads %SERVER_THREADS%
start "AsynGyanis soak server" /MIN "%SERVER_PATH%" --port %SERVER_PORT% --threads %SERVER_THREADS%

rem 等端口就绪：最多 30 秒，每秒试一次
set SERVER_PID=
for /l %%i in (1,1,30) do (
    if not defined SERVER_PID (
        for /f "tokens=2 delims=," %%p in ('tasklist /FI "IMAGENAME eq echo_server.exe" /FO CSV /NH 2^>nul') do set SERVER_PID=%%~p
        if not defined SERVER_PID timeout /t 1 /nobreak >nul
    )
)
if not defined SERVER_PID (
    echo 服务器未在 30 秒内启动，放弃。
    exit /b 1
)
echo 服务器进程号 %SERVER_PID%

python "%REPOSITORY_ROOT%\benchmarks\soak.py" --port %SERVER_PORT% --pid %SERVER_PID% %3 %4 %5 %6 %7 %8 %9
set SOAK_EXIT_CODE=%ERRORLEVEL%

rem 压测脚本按「主动收口」的语义结束：这里直接结束进程（服务器自身的优雅 drain 由 samples/main.cpp 演示）
taskkill /F /PID %SERVER_PID% >nul 2>&1

echo 压测结束，脚本退出码 %SOAK_EXIT_CODE%（0 表示协议与负载零失败）
exit /b %SOAK_EXIT_CODE%
