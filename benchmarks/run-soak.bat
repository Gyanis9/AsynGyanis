@echo off
rem ============================================================================
rem 进程外压测：启动示例服务器 → 跑 benchmarks/soak.py → 收工。
rem 用法：benchmarks\run-soak.bat <端口> [工作线程数] [压测脚本附加参数...]
rem 前提：先构建过对应构建类型的 echo_server。
rem 构建类型：默认 debug（build\debug）；设 ASYN_SOAK_BUILD=release 换成 build\release。
rem        Debug 带 AddressSanitizer，吞吐只用于同类构建的回归对比，不是性能上限；
rem        与 benchmarks/baseline.json 比对必须用 release，那份基线就是 Release 下测的。
rem 门禁用法：... run-soak.bat 18080 4 --json-out build\soak.json
rem        然后 python benchmarks\check-baseline.py build\soak.json
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
    echo 找不到 %SERVER_PATH%，请先构建 %ASYN_SOAK_BUILD% 目标 echo_server。
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
