# Net/Acme —— 证书自动化（RFC 8555 客户端与续期循环）
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/AcmeError.h
        ${CMAKE_CURRENT_LIST_DIR}/AcmeError.cpp
        ${CMAKE_CURRENT_LIST_DIR}/AcmeKeyPair.h
        ${CMAKE_CURRENT_LIST_DIR}/AcmeKeyPair.cpp
        ${CMAKE_CURRENT_LIST_DIR}/AcmeClient.h
        ${CMAKE_CURRENT_LIST_DIR}/AcmeClient.cpp
)
