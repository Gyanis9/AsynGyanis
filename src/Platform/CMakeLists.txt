add_library(Platform INTERFACE)

target_include_directories(Platform INTERFACE
        ${CMAKE_SOURCE_DIR}/src
)

if(WIN32)
    target_link_libraries(Platform INTERFACE
            ws2_32
            Mswsock
    )
endif()
