# Core/Process —— 进程侧的编排与接管：多进程 worker 的启停看护、停机信号的接管，以及换代交接通道
set_property(GLOBAL APPEND PROPERTY AsynGyanisCoreSources
        ${CMAKE_CURRENT_LIST_DIR}/WorkerSupervisor.h
        ${CMAKE_CURRENT_LIST_DIR}/WorkerSupervisor.cpp
        ${CMAKE_CURRENT_LIST_DIR}/GracefulShutdown.h
        ${CMAKE_CURRENT_LIST_DIR}/GracefulShutdown.cpp
        ${CMAKE_CURRENT_LIST_DIR}/UpgradeChannel.h
        ${CMAKE_CURRENT_LIST_DIR}/UpgradeChannel.cpp
)
