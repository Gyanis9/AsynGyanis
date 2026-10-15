# Core/Crypto —— 摘要与消息认证码、密码学随机源、HKDF（都是 OpenSSL EVP 的一次性接口薄封装）
set_property(GLOBAL APPEND PROPERTY AsynGyanisCoreSources
        ${CMAKE_CURRENT_LIST_DIR}/Digest.h
        ${CMAKE_CURRENT_LIST_DIR}/Digest.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Random.h
        ${CMAKE_CURRENT_LIST_DIR}/Random.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Hkdf.h
        ${CMAKE_CURRENT_LIST_DIR}/Hkdf.cpp
)
