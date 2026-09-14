# Core/Process —— 进程编排：多进程 worker 的启停与看护
set_property(GLOBAL APPEND PROPERTY AsynGyanisCoreSources
        ${CMAKE_CURRENT_LIST_DIR}/WorkerSupervisor.h
        ${CMAKE_CURRENT_LIST_DIR}/WorkerSupervisor.cpp
)
