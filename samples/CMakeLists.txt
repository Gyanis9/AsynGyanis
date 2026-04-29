add_executable(echo_server main.cpp)

target_link_libraries(echo_server PRIVATE
        Base
        Core
        Net
)

target_include_directories(echo_server PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(echo_server PRIVATE cxx_std_20)
