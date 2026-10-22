# Database/Queryable —— ORM 查询树核心子系统（编译期生成器，无数据库 IO）
set_property(GLOBAL APPEND PROPERTY AsynGyanisDatabaseSources
        ${CMAKE_CURRENT_LIST_DIR}/Column.h
        ${CMAKE_CURRENT_LIST_DIR}/TableSchema.h
        ${CMAKE_CURRENT_LIST_DIR}/QueryNode.h
        ${CMAKE_CURRENT_LIST_DIR}/Relations.h
        ${CMAKE_CURRENT_LIST_DIR}/Expression.h
        ${CMAKE_CURRENT_LIST_DIR}/Queryable.h
        # 只有实现：Queryable 的私有模板方法定义由公开头在类定义之后包含
        ${CMAKE_CURRENT_LIST_DIR}/QueryableImpl.h
        ${CMAKE_CURRENT_LIST_DIR}/SchemaMigrator.h
        # 只有实现：SchemaMigrator 的模板方法定义由公开头在类定义之后包含
        ${CMAKE_CURRENT_LIST_DIR}/SchemaMigratorImpl.h
        ${CMAKE_CURRENT_LIST_DIR}/RowMapper.h
        ${CMAKE_CURRENT_LIST_DIR}/Queryable.cpp
)