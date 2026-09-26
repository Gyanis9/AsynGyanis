# Core/Process —— 进程侧的编排与接管：多进程 worker 的启停看护，以及停机信号的接管
set_property(GLOBAL APPEND PROPERTY AsynGyanisCoreSources
        ${CMAKE_CURRENT_LIST_DIR}/WorkerSupervisor.h
        ${CMAKE_CURRENT_LIST_DIR}/WorkerSupervisor.cpp
        ${CMAKE_CURRENT_LIST_DIR}/GracefulShutdown.h
        ${CMAKE_CURRENT_LIST_DIR}/GracefulShutdown.cpp
)
