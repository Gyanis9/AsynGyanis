# Platform/System —— 操作系统信息：错误码、本地时间、进程信息、子进程、文本编码
set_property(GLOBAL APPEND PROPERTY AsynGyanisPlatformSources
        ${CMAKE_CURRENT_LIST_DIR}/PlatformError.h
        ${CMAKE_CURRENT_LIST_DIR}/PlatformError.cpp
        ${CMAKE_CURRENT_LIST_DIR}/PlatformTime.h
        ${CMAKE_CURRENT_LIST_DIR}/PlatformTime.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Process.h
        ${CMAKE_CURRENT_LIST_DIR}/Process.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ProcessInfo.h
        ${CMAKE_CURRENT_LIST_DIR}/ProcessInfo.cpp
        ${CMAKE_CURRENT_LIST_DIR}/TextEncoding.h
        ${CMAKE_CURRENT_LIST_DIR}/TextEncoding.cpp
)
