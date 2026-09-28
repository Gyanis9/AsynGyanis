# Net/Udp —— 面向任意来源的数据报服务端（一条端口、逐条交付、按来源回包）
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        ${CMAKE_CURRENT_LIST_DIR}/UdpServer.h
        ${CMAKE_CURRENT_LIST_DIR}/UdpServer.cpp
)
