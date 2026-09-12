# Platform/IO —— 描述符、socket、事件通知、定时器与控制台交互
set_property(GLOBAL APPEND PROPERTY AsynGyanisPlatformSources
        ${CMAKE_CURRENT_LIST_DIR}/FileDescriptor.h
        ${CMAKE_CURRENT_LIST_DIR}/FileDescriptor.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Socket.h
        ${CMAKE_CURRENT_LIST_DIR}/Socket.cpp
        ${CMAKE_CURRENT_LIST_DIR}/EventNotifier.h
        ${CMAKE_CURRENT_LIST_DIR}/EventNotifier.cpp
        ${CMAKE_CURRENT_LIST_DIR}/TimerFileDescriptor.h
        ${CMAKE_CURRENT_LIST_DIR}/TimerFileDescriptor.cpp
        ${CMAKE_CURRENT_LIST_DIR}/MemoryMappedFile.h
        ${CMAKE_CURRENT_LIST_DIR}/MemoryMappedFile.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Console.h
        ${CMAKE_CURRENT_LIST_DIR}/Console.cpp
)
