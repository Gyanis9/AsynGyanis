# Database/Pool —— 高性能连接池子系统
set_property(GLOBAL APPEND PROPERTY AsynGyanisDatabaseSources
        ${CMAKE_CURRENT_LIST_DIR}/PoolConfig.h
        ${CMAKE_CURRENT_LIST_DIR}/PoolLiveness.h
        ${CMAKE_CURRENT_LIST_DIR}/PooledConnection.h
        ${CMAKE_CURRENT_LIST_DIR}/PooledConnection.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ConnectionPool.h
        ${CMAKE_CURRENT_LIST_DIR}/ConnectionPool.cpp
        ${CMAKE_CURRENT_LIST_DIR}/AsyncExecutor.h
        ${CMAKE_CURRENT_LIST_DIR}/AsyncExecutor.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Transaction.h
        ${CMAKE_CURRENT_LIST_DIR}/Transaction.cpp
)