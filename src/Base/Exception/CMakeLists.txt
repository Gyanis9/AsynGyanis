# Base/Exception —— 项目统一异常体系，一个异常类一个文件
set_property(GLOBAL APPEND PROPERTY AsynGyanisBaseSources
        ${CMAKE_CURRENT_LIST_DIR}/Exception.h
        ${CMAKE_CURRENT_LIST_DIR}/Exception.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ExceptionMessage.h
        ${CMAKE_CURRENT_LIST_DIR}/StackTrace.h
        ${CMAKE_CURRENT_LIST_DIR}/StackTrace.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ExceptionStackTrace.h
        ${CMAKE_CURRENT_LIST_DIR}/LogicException.h
        ${CMAKE_CURRENT_LIST_DIR}/LogicException.cpp
        ${CMAKE_CURRENT_LIST_DIR}/InvalidArgumentException.h
        ${CMAKE_CURRENT_LIST_DIR}/InvalidArgumentException.cpp
        ${CMAKE_CURRENT_LIST_DIR}/SystemException.h
        ${CMAKE_CURRENT_LIST_DIR}/SystemException.cpp
        ${CMAKE_CURRENT_LIST_DIR}/NetworkException.h
        ${CMAKE_CURRENT_LIST_DIR}/NetworkException.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ConfigException.h
        ${CMAKE_CURRENT_LIST_DIR}/ConfigException.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ConfigFileException.h
        ${CMAKE_CURRENT_LIST_DIR}/ConfigFileException.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ConfigParseException.h
        ${CMAKE_CURRENT_LIST_DIR}/ConfigParseException.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ConfigKeyNotFoundException.h
        ${CMAKE_CURRENT_LIST_DIR}/ConfigKeyNotFoundException.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ConfigValidationException.h
        ${CMAKE_CURRENT_LIST_DIR}/ConfigValidationException.cpp
)
