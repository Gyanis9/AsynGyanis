# Database/Common —— 数据库抽象契约、值类型与工厂
set_property(GLOBAL APPEND PROPERTY AsynGyanisDatabaseSources
        ${CMAKE_CURRENT_LIST_DIR}/DatabaseType.h
        ${CMAKE_CURRENT_LIST_DIR}/DatabaseType.cpp
        ${CMAKE_CURRENT_LIST_DIR}/DatabaseValue.h
        ${CMAKE_CURRENT_LIST_DIR}/DatabaseValue.cpp
        ${CMAKE_CURRENT_LIST_DIR}/BinaryBytes.h
        ${CMAKE_CURRENT_LIST_DIR}/ConnectionConfig.h
        ${CMAKE_CURRENT_LIST_DIR}/DatabaseResult.h
        ${CMAKE_CURRENT_LIST_DIR}/DatabaseConnection.h
        ${CMAKE_CURRENT_LIST_DIR}/DatabaseFactory.h
        ${CMAKE_CURRENT_LIST_DIR}/DatabaseFactory.cpp
        # 数据库模块异常体系：一个类一个文件（运行期故障派生自 Base::Exception）
        ${CMAKE_CURRENT_LIST_DIR}/DatabaseException.h
        ${CMAKE_CURRENT_LIST_DIR}/DatabaseException.cpp
        ${CMAKE_CURRENT_LIST_DIR}/ConnectionUnavailableException.h
        ${CMAKE_CURRENT_LIST_DIR}/ConnectionUnavailableException.cpp
        ${CMAKE_CURRENT_LIST_DIR}/QueryExecutionException.h
        ${CMAKE_CURRENT_LIST_DIR}/QueryExecutionException.cpp
        ${CMAKE_CURRENT_LIST_DIR}/RowMappingException.h
        ${CMAKE_CURRENT_LIST_DIR}/RowMappingException.cpp
)
