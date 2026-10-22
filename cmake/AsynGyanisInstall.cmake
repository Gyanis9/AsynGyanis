# 安装规则与 CMake 包配置：模块库、公开头、能力清单与 find_package 支持。
include_guard(GLOBAL)

if(ASYN_ENABLE_INSTALL)
    include(CMakePackageConfigHelpers)

    # 每个模块单独标注组件名；install(TARGETS/EXPORT) 不支持复数 COMPONENTS。
    install(TARGETS Platform EXPORT AsynGyanisTargets COMPONENT Platform
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(TARGETS Base EXPORT AsynGyanisTargets COMPONENT Base
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(TARGETS Core EXPORT AsynGyanisTargets COMPONENT Core
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(TARGETS Net EXPORT AsynGyanisTargets COMPONENT Net
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(TARGETS Database EXPORT AsynGyanisTargets COMPONENT Database
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    # 头文件保持 src 相对路径；排除实现侧头，避免把内部结构带进对外契约。
    # 判据：没有已安装的公开头包含它们；ConfigManager.h 依赖的 Detail/ReloadRoundGate.h 不排除。
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/src/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            FILES_MATCHING PATTERN "*.h"
            PATTERN "MySqlValueConversion.h" EXCLUDE
            PATTERN "MySqlResult.h" EXCLUDE
            PATTERN "RedisReplyText.h" EXCLUDE
            PATTERN "PlainTextLogLine.h" EXCLUDE
            PATTERN "RollingPeriod.h" EXCLUDE
            PATTERN "OAuthSupport.h" EXCLUDE
            PATTERN "HttpProxyPumpedTls.h" EXCLUDE
            PATTERN "SignalObserverInstall.h" EXCLUDE
            PATTERN "WakeupFailureReport.h" EXCLUDE
            PATTERN "WorkerSupervisorSupport.h" EXCLUDE
            PATTERN "ConfigDocument.h" EXCLUDE
            PATTERN "ConfigEnvironmentOverride.h" EXCLUDE
            PATTERN "Http2ErrorOut.h" EXCLUDE
            PATTERN "QuicSupport.h" EXCLUDE
            PATTERN "OtlpHttpExportSupport.h" EXCLUDE
            PATTERN "OtlpJsonSupport.h" EXCLUDE
            PATTERN "OtlpConfigurationSupport.h" EXCLUDE
            PATTERN "HttpConfigDuration.h" EXCLUDE
            PATTERN "HttpConfigValidation.h" EXCLUDE
            PATTERN "HttpEndpointPath.h" EXCLUDE
            PATTERN "TlsPeerIdentitySupport.h" EXCLUDE
            PATTERN "WebSocketUpgradeAdmission.h" EXCLUDE
            PATTERN "HttpClientUrl.h" EXCLUDE
            PATTERN "HttpClientSupport.h" EXCLUDE
            PATTERN "Http2SessionSupport.h" EXCLUDE
            PATTERN "Http3SessionSupport.h" EXCLUDE
            PATTERN "RedisConnectionSupport.h" EXCLUDE
            PATTERN "SchemaMigratorImpl.h" EXCLUDE
            PATTERN "QueryableImpl.h" EXCLUDE
    )

    # Base 的 std::stacktrace 探测结果随包交出；Conan 消费方需要同一个宏。
    install(FILES ${CMAKE_BINARY_DIR}/src/Base/AsynGyanisStacktraceSupport.txt
            DESTINATION ${CMAKE_INSTALL_DATADIR}/asyngyanis
    )

    # 能力清单头把构建开关变成 0/1 宏；改名避免与使用方同名宏冲突。
    # 必须在 tests 之后生成：ASYN_BUILD_FUZZ_TARGETS 定义在 tests/Net。
    set(ASYN_HAS_IO_URING ${ASYN_WITH_IO_URING})
    set(ASYN_HAS_MIMALLOC ${ASYN_WITH_MIMALLOC})
    set(ASYN_IS_SANITIZED ${ENABLE_SANITIZERS})
    set(ASYN_SANITIZES_THREADS ${SANITIZE_THREADS})
    # 覆盖计数取事实：CI 的 coverage 档用裸 --coverage，没给 ASYN_ENABLE_COVERAGE。
    if(ASYN_ENABLE_COVERAGE OR CMAKE_CXX_FLAGS MATCHES "--coverage" OR CMAKE_C_FLAGS MATCHES "--coverage")
        set(ASYN_HAS_COVERAGE ON)
    else()
        set(ASYN_HAS_COVERAGE OFF)
    endif()
    set(ASYN_IS_SHARED_BUILD ${BUILD_SHARED_LIBS})
    set(ASYN_HAS_FUZZ_TARGETS ${ASYN_BUILD_FUZZ_TARGETS})
    # 这两项改变使用方写代码的方式，取值直接读模块实际导出的编译定义。
    get_target_property(asynDatabaseExportedDefinitions Database INTERFACE_COMPILE_DEFINITIONS)
    get_target_property(asynBaseExportedDefinitions Base INTERFACE_COMPILE_DEFINITIONS)
    if(NOT asynDatabaseExportedDefinitions)
        set(asynDatabaseExportedDefinitions "")
    endif()
    if(NOT asynBaseExportedDefinitions)
        set(asynBaseExportedDefinitions "")
    endif()
    # 可选驱动由 Database 模块登记；空表直接拒绝，避免 configure_file 静默落成 0。
    get_property(asynDatabaseDrivers GLOBAL PROPERTY AsynGyanisDatabaseDrivers)
    if(NOT asynDatabaseDrivers)
        message(FATAL_ERROR "Database 模块没有登记任何可选驱动（AsynGyanisDatabaseDrivers 为空）：检查 src 的 add_subdirectory 顺序或驱动清单")
    endif()
    foreach(asynDriverName IN LISTS asynDatabaseDrivers)
        if(asynDatabaseExportedDefinitions MATCHES "DATABASE_HAS_${asynDriverName}=1")
            set(ASYN_HAS_${asynDriverName}_DRIVER ON)
        else()
            set(ASYN_HAS_${asynDriverName}_DRIVER OFF)
        endif()
    endforeach()
    if(asynBaseExportedDefinitions MATCHES "ASYN_HAS_STACKTRACE=1")
        set(ASYN_HAS_STACKTRACE_SUPPORT ON)
    else()
        set(ASYN_HAS_STACKTRACE_SUPPORT OFF)
    endif()
    configure_file(${CMAKE_SOURCE_DIR}/cmake/AsynGyanisFeatures.h.in
                   ${CMAKE_BINARY_DIR}/src/AsynGyanisFeatures.h
                   @ONLY)
    install(FILES ${CMAKE_BINARY_DIR}/src/AsynGyanisFeatures.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    # 组件依赖图；外部依赖由各模块登记进 GLOBAL 属性，根作用域看不见 imported 目标。
    set(ASYN_PACKAGE_COMPONENTS Platform Base Core Net Database)
    set(ASYN_COMPONENT_REQUIRES_Base Platform)
    set(ASYN_COMPONENT_REQUIRES_Core Base)
    set(ASYN_COMPONENT_REQUIRES_Net Core)
    set(ASYN_COMPONENT_REQUIRES_Database Core)

    # 为每个组件求「本身 + 传递依赖组件」的外部依赖并集。
    set(ASYN_CONFIG_COMPONENT_DEPENDENCIES "")
    set(ASYN_CONFIG_ALL_DEPENDENCIES "")
    foreach(component IN LISTS ASYN_PACKAGE_COMPONENTS)
        set(pendingComponents "${component};${ASYN_COMPONENT_REQUIRES_${component}}")
        set(visitedComponents "")
        set(componentDependencies "")
        while(pendingComponents)
            list(POP_FRONT pendingComponents currentComponent)
            if(currentComponent IN_LIST visitedComponents)
                continue()
            endif()
            list(APPEND visitedComponents "${currentComponent}")
            get_property(currentDependencies GLOBAL PROPERTY AsynGyanisPackageDependencies_${currentComponent})
            list(APPEND componentDependencies ${currentDependencies})
            list(APPEND pendingComponents ${ASYN_COMPONENT_REQUIRES_${currentComponent}})
        endwhile()
        list(REMOVE_DUPLICATES componentDependencies)
        string(APPEND ASYN_CONFIG_COMPONENT_DEPENDENCIES "set(AsynGyanis_${component}_DEPENDENCIES ${componentDependencies})\n")
        list(APPEND ASYN_CONFIG_ALL_DEPENDENCIES ${componentDependencies})
    endforeach()
    list(REMOVE_DUPLICATES ASYN_CONFIG_ALL_DEPENDENCIES)
    # 全量清单在模板里当字面量用；未请求组件时走这条，保持旧行为。
    string(REPLACE ";" " " ASYN_CONFIG_ALL_DEPENDENCIES "${ASYN_CONFIG_ALL_DEPENDENCIES}")

    # 容器注解开关是 ABI 契约；Debug + MSVC 下随导出接口带出，否则消费方链接期 LNK2038。
    if(MSVC AND ENABLE_SANITIZERS)
        foreach(asyn_module Platform Base Core Net Database)
            target_compile_definitions(${asyn_module} INTERFACE
                    $<$<CONFIG:Debug>:_DISABLE_VECTOR_ANNOTATION>
                    $<$<CONFIG:Debug>:_DISABLE_STRING_ANNOTATION>
                    $<$<CONFIG:Debug>:_DISABLE_OPTIONAL_ANNOTATION>
            )
        endforeach()
    endif()

    configure_package_config_file(
            ${CMAKE_SOURCE_DIR}/cmake/AsynGyanisConfig.cmake.in
            ${CMAKE_CURRENT_BINARY_DIR}/AsynGyanisConfig.cmake
            INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AsynGyanis
    )

    # 版本号取自 project()，同一主版本内兼容。
    write_basic_package_version_file(
            ${CMAKE_CURRENT_BINARY_DIR}/AsynGyanisConfigVersion.cmake
            VERSION ${PROJECT_VERSION}
            COMPATIBILITY SameMajorVersion
    )

    install(EXPORT AsynGyanisTargets
            FILE AsynGyanisTargets.cmake
            NAMESPACE AsynGyanis::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AsynGyanis
    )

    install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/AsynGyanisConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/AsynGyanisConfigVersion.cmake
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AsynGyanis
    )
endif()
