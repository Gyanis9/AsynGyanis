find_package(Catch2 REQUIRED)

add_executable(TestDatabase
    TestDatabaseCore.cpp
    TestSqliteDatabase.cpp
)

target_link_libraries(TestDatabase PRIVATE
    Database
    Catch2::Catch2WithMain
)

target_include_directories(TestDatabase PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(TestDatabase PRIVATE cxx_std_20)

include(Catch)
catch_discover_tests(TestDatabase)
