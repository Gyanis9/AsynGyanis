# Database/Sqlite —— SQLite 嵌入式驱动
set_property(GLOBAL APPEND PROPERTY AsynGyanisDatabaseSources
        ${CMAKE_CURRENT_LIST_DIR}/SqliteConnection.h
        ${CMAKE_CURRENT_LIST_DIR}/SqliteConnection.cpp
        ${CMAKE_CURRENT_LIST_DIR}/SqliteResult.h
        ${CMAKE_CURRENT_LIST_DIR}/SqliteResult.cpp
)
