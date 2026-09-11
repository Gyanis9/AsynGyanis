# Database/MySql —— MySQL / MariaDB 驱动（依赖 libmysqlclient，缺失时编译报错桩）
set_property(GLOBAL APPEND PROPERTY AsynGyanisDatabaseSources
        ${CMAKE_CURRENT_LIST_DIR}/MySqlConnection.h
        ${CMAKE_CURRENT_LIST_DIR}/MySqlConnection.cpp
        ${CMAKE_CURRENT_LIST_DIR}/MySqlResult.h
        ${CMAKE_CURRENT_LIST_DIR}/MySqlResult.cpp
)
