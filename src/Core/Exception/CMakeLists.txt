# Core/Exception —— Core 模块的异常体系，一个异常类一个文件
set_property(GLOBAL APPEND PROPERTY AsynGyanisCoreSources
        ${CMAKE_CURRENT_LIST_DIR}/CoreException.h
        ${CMAKE_CURRENT_LIST_DIR}/CoreException.cpp
)
