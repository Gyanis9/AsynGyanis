# Database/Redis —— Redis 驱动（依赖 hiredis，缺失时编译报错桩）
set_property(GLOBAL APPEND PROPERTY AsynGyanisDatabaseSources
        ${CMAKE_CURRENT_LIST_DIR}/RedisConnection.h
        ${CMAKE_CURRENT_LIST_DIR}/RedisConnection.cpp
        ${CMAKE_CURRENT_LIST_DIR}/RedisResult.h
        ${CMAKE_CURRENT_LIST_DIR}/RedisResult.cpp
        ${CMAKE_CURRENT_LIST_DIR}/RedisReplyText.h
)
