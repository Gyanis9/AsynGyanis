# Platform/FileSystem —— 文件与目录：路径构造、原子写、文件监听
set_property(GLOBAL APPEND PROPERTY AsynGyanisPlatformSources
        ${CMAKE_CURRENT_LIST_DIR}/FileSystem.h
        ${CMAKE_CURRENT_LIST_DIR}/FileSystem.cpp
        ${CMAKE_CURRENT_LIST_DIR}/FileBasicInfo.h
        ${CMAKE_CURRENT_LIST_DIR}/FileBasicInfo.cpp
        ${CMAKE_CURRENT_LIST_DIR}/AtomicFileWriter.h
        ${CMAKE_CURRENT_LIST_DIR}/AtomicFileWriter.cpp
        ${CMAKE_CURRENT_LIST_DIR}/FileWatcher.h
        ${CMAKE_CURRENT_LIST_DIR}/FileWatcher.cpp
)

# 文件监听器按平台各自提供唯一实现，另一平台的实现不参与编译
if(WIN32)
    set_property(GLOBAL APPEND PROPERTY AsynGyanisPlatformSources
            ${CMAKE_CURRENT_LIST_DIR}/Win32FileWatcher.h
            ${CMAKE_CURRENT_LIST_DIR}/Win32FileWatcher.cpp
    )
else()
    set_property(GLOBAL APPEND PROPERTY AsynGyanisPlatformSources
            ${CMAKE_CURRENT_LIST_DIR}/InotifyFileWatcher.h
            ${CMAKE_CURRENT_LIST_DIR}/InotifyFileWatcher.cpp
    )
endif()
