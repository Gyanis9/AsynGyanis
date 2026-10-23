cmake_minimum_required(VERSION 3.20)

project(AsynGyanisConsumerJourney LANGUAGES CXX)

# 消费方视角：只认 find_package 交出来的目标，不引用仓库内的任何路径；版本判据来自包元数据
# （AsynGyanisConfigVersion.cmake），由 configure 期读出后钉进编译定义。Core/Platform 供多进程
# 网关那一步直接使用（WorkerSupervisor/GracefulShutdown/AsyncSocket），仍只经 find_package 拿目标
find_package(AsynGyanis REQUIRED COMPONENTS Net Database Base Core Platform)

add_executable(consumer_journey main.cpp)
target_link_libraries(consumer_journey PRIVATE AsynGyanis::Net AsynGyanis::Database AsynGyanis::Base AsynGyanis::Core AsynGyanis::Platform)
target_compile_features(consumer_journey PRIVATE cxx_std_23)

if (DEFINED JOURNEY_EXPECTED_VERSION)
    target_compile_definitions(consumer_journey PRIVATE JOURNEY_EXPECTED_VERSION="${JOURNEY_EXPECTED_VERSION}")
endif ()
