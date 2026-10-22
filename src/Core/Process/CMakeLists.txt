# Core/Process —— 进程编排与接管：worker 启停看护、停机与重载信号接管、服务管理器看门狗
# 节拍、换代交接通道与崩溃现场记录
set_property(GLOBAL APPEND PROPERTY AsynGyanisCoreSources
        ${CMAKE_CURRENT_LIST_DIR}/WorkerSupervisor.h
        ${CMAKE_CURRENT_LIST_DIR}/WorkerSupervisor.cpp
        ${CMAKE_CURRENT_LIST_DIR}/WorkerSupervisorLaunch.cpp
        ${CMAKE_CURRENT_LIST_DIR}/WorkerSupervisorWatchdog.cpp
        ${CMAKE_CURRENT_LIST_DIR}/WorkerSupervisorSupport.h
        ${CMAKE_CURRENT_LIST_DIR}/GracefulShutdown.h
        ${CMAKE_CURRENT_LIST_DIR}/GracefulShutdown.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ReloadSignal.h
        ${CMAKE_CURRENT_LIST_DIR}/ReloadSignal.cpp
        ${CMAKE_CURRENT_LIST_DIR}/SignalObserverInstall.h
        ${CMAKE_CURRENT_LIST_DIR}/SignalObserverInstall.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ServiceWatchdog.h
        ${CMAKE_CURRENT_LIST_DIR}/ServiceWatchdog.cpp
        ${CMAKE_CURRENT_LIST_DIR}/RoundTicker.h
        ${CMAKE_CURRENT_LIST_DIR}/RoundTicker.cpp
        ${CMAKE_CURRENT_LIST_DIR}/WorkerHeartbeat.h
        ${CMAKE_CURRENT_LIST_DIR}/WorkerHeartbeat.cpp
        ${CMAKE_CURRENT_LIST_DIR}/UpgradeChannel.h
        ${CMAKE_CURRENT_LIST_DIR}/UpgradeChannel.cpp
        ${CMAKE_CURRENT_LIST_DIR}/CrashScene.h
        ${CMAKE_CURRENT_LIST_DIR}/CrashScene.cpp
)
