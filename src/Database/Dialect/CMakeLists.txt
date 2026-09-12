# Database/Dialect —— SQL 方言层（查询树 → 参数化 SQL，纯翻译无数据库 IO）
set_property(GLOBAL APPEND PROPERTY AsynGyanisDatabaseSources
        ${CMAKE_CURRENT_LIST_DIR}/SqlStatement.h
        ${CMAKE_CURRENT_LIST_DIR}/SqlDialect.h
        ${CMAKE_CURRENT_LIST_DIR}/SqliteDialect.h
        ${CMAKE_CURRENT_LIST_DIR}/SqliteDialect.cpp
        ${CMAKE_CURRENT_LIST_DIR}/MySqlDialect.h
        ${CMAKE_CURRENT_LIST_DIR}/MySqlDialect.cpp
        ${CMAKE_CURRENT_LIST_DIR}/DialectRegistry.h
        ${CMAKE_CURRENT_LIST_DIR}/DialectRegistry.cpp
)
