# Core/Coroutine —— 协程任务、调度器、协程池与并发原语
set_property(GLOBAL APPEND PROPERTY AsynGyanisCoreSources
        ${CMAKE_CURRENT_LIST_DIR}/Task.h
        ${CMAKE_CURRENT_LIST_DIR}/DeadlineGuard.h
        ${CMAKE_CURRENT_LIST_DIR}/Scheduler.h
        ${CMAKE_CURRENT_LIST_DIR}/Scheduler.cpp
        ${CMAKE_CURRENT_LIST_DIR}/CoroutinePool.h
        ${CMAKE_CURRENT_LIST_DIR}/CoroutinePool.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Cancelable.h
        ${CMAKE_CURRENT_LIST_DIR}/ThreadPool.h
        ${CMAKE_CURRENT_LIST_DIR}/ThreadPool.cpp
        ${CMAKE_CURRENT_LIST_DIR}/AsyncExecutor.h
        ${CMAKE_CURRENT_LIST_DIR}/AsyncExecutor.cpp
)
