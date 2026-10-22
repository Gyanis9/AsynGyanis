# Database/Redis —— Redis 驱动（依赖 hiredis，缺失时编译报错桩）
set_property(GLOBAL APPEND PROPERTY AsynGyanisDatabaseSources
        ${CMAKE_CURRENT_LIST_DIR}/RedisConnection.h
        ${CMAKE_CURRENT_LIST_DIR}/RedisConnection.cpp
        # 订阅/退订/推送读取；时长换算头只给实现侧
        ${CMAKE_CURRENT_LIST_DIR}/RedisConnectionSupport.h
        ${CMAKE_CURRENT_LIST_DIR}/RedisSubscription.cpp
        ${CMAKE_CURRENT_LIST_DIR}/RedisResult.h
        ${CMAKE_CURRENT_LIST_DIR}/RedisResult.cpp
        ${CMAKE_CURRENT_LIST_DIR}/RedisReplyText.h
        ${CMAKE_CURRENT_LIST_DIR}/RedisSentinel.h
        ${CMAKE_CURRENT_LIST_DIR}/RedisSentinel.cpp
)
