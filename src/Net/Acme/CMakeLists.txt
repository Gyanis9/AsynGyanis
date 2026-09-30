# Net/Acme —— 证书自动化（RFC 8555 客户端与续期循环）
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/AcmeError.h
        ${CMAKE_CURRENT_LIST_DIR}/AcmeError.cpp
        ${CMAKE_CURRENT_LIST_DIR}/AcmeKeyPair.h
        ${CMAKE_CURRENT_LIST_DIR}/AcmeKeyPair.cpp
        ${CMAKE_CURRENT_LIST_DIR}/AcmeClient.h
        ${CMAKE_CURRENT_LIST_DIR}/AcmeClient.cpp
        ${CMAKE_CURRENT_LIST_DIR}/AcmeHttp01ChallengeStore.h
        ${CMAKE_CURRENT_LIST_DIR}/AcmeHttp01ChallengeStore.cpp
        ${CMAKE_CURRENT_LIST_DIR}/AcmeDns01TxtWriter.h
        ${CMAKE_CURRENT_LIST_DIR}/AcmeAliyunDns01TxtWriter.h
        ${CMAKE_CURRENT_LIST_DIR}/AcmeAliyunDns01TxtWriter.cpp
        ${CMAKE_CURRENT_LIST_DIR}/AcmeAutomationConfig.h
        ${CMAKE_CURRENT_LIST_DIR}/AcmeAutomationConfig.cpp
        ${CMAKE_CURRENT_LIST_DIR}/AcmeCertificateManager.h
        ${CMAKE_CURRENT_LIST_DIR}/AcmeCertificateManager.cpp
)
