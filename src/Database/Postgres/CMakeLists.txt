# Database/Postgres —— PostgreSQL 驱动（依赖 libpq，缺失时编译报错桩）
set_property(GLOBAL APPEND PROPERTY AsynGyanisDatabaseSources
        ${CMAKE_CURRENT_LIST_DIR}/PostgresConnection.h
        ${CMAKE_CURRENT_LIST_DIR}/PostgresConnection.cpp
        ${CMAKE_CURRENT_LIST_DIR}/PostgresResult.h
        ${CMAKE_CURRENT_LIST_DIR}/PostgresResult.cpp
        ${CMAKE_CURRENT_LIST_DIR}/PostgresValueConversion.h
)
