# Platform 按职责分三个子目录：IO（描述符与设备）、FileSystem（文件与目录）、System（操作系统信息）
# 子目录仅作物理组织，命名空间统一为 AsynGyanis::Platform
add_library(Platform STATIC
        Platform.h

        IO/FileDescriptor.h
        IO/FileDescriptor.cpp
        IO/Socket.h
        IO/Socket.cpp
        IO/EventNotifier.h
        IO/EventNotifier.cpp
        IO/TimerFileDescriptor.h
        IO/TimerFileDescriptor.cpp
        IO/Console.h
        IO/Console.cpp

        FileSystem/FileSystem.h
        FileSystem/FileSystem.cpp
        FileSystem/AtomicFileWriter.h
        FileSystem/AtomicFileWriter.cpp
        FileSystem/FileWatcher.h
        FileSystem/FileWatcher.cpp

        System/PlatformError.h
        System/PlatformError.cpp
        System/PlatformTime.h
        System/PlatformTime.cpp
        System/ProcessInfo.h
        System/ProcessInfo.cpp
        System/TextEncoding.h
        System/TextEncoding.cpp
)

# 文件监听器按平台各自提供唯一实现，另一平台的源文件不参与编译
if (WIN32)
    target_sources(Platform PRIVATE
            FileSystem/Win32FileWatcher.h
            FileSystem/Win32FileWatcher.cpp
    )
    target_link_libraries(Platform PUBLIC
            ws2_32
            Mswsock
    )
else ()
    target_sources(Platform PRIVATE
            FileSystem/InotifyFileWatcher.h
            FileSystem/InotifyFileWatcher.cpp
    )
endif ()

find_package(Threads REQUIRED)

target_link_libraries(Platform PUBLIC
        Threads::Threads
)

target_include_directories(Platform PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(Platform PUBLIC cxx_std_20)
