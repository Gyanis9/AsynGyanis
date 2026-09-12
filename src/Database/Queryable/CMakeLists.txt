# Database/Queryable —— ORM 查询树核心子系统（编译期生成器，无数据库 IO）
set_property(GLOBAL APPEND PROPERTY AsynGyanisDatabaseSources
        ${CMAKE_CURRENT_LIST_DIR}/Column.h
        ${CMAKE_CURRENT_LIST_DIR}/TableSchema.h
        ${CMAKE_CURRENT_LIST_DIR}/QueryNode.h
        ${CMAKE_CURRENT_LIST_DIR}/Expression.h
        ${CMAKE_CURRENT_LIST_DIR}/Queryable.h
        ${CMAKE_CURRENT_LIST_DIR}/SchemaMigrator.h
        ${CMAKE_CURRENT_LIST_DIR}/RowMapper.h
        ${CMAKE_CURRENT_LIST_DIR}/Queryable.cpp
)