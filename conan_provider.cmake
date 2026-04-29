set(CONAN_MINIMUM_VERSION 2.0.5)
cmake_policy(PUSH)
cmake_minimum_required(VERSION 3.24)


function(detect_os os os_api_level os_sdk os_subsystem os_version)
    message(STATUS "CMake-Conan: cmake_system_name=${CMAKE_SYSTEM_NAME}")
    if (CMAKE_SYSTEM_NAME AND NOT CMAKE_SYSTEM_NAME STREQUAL "Generic")
        if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
            set(${os} Macos PARENT_SCOPE)
        elseif (CMAKE_SYSTEM_NAME STREQUAL "QNX")
            set(${os} Neutrino PARENT_SCOPE)
        elseif (CMAKE_SYSTEM_NAME STREQUAL "CYGWIN")
            set(${os} Windows PARENT_SCOPE)
            set(${os_subsystem} cygwin PARENT_SCOPE)
        elseif (CMAKE_SYSTEM_NAME MATCHES "^MSYS")
            set(${os} Windows PARENT_SCOPE)
            set(${os_subsystem} msys2 PARENT_SCOPE)
        elseif (CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
            set(${os} Emscripten PARENT_SCOPE)
        else ()
            set(${os} ${CMAKE_SYSTEM_NAME} PARENT_SCOPE)
        endif ()
        if (CMAKE_SYSTEM_NAME STREQUAL "Android")
            if (DEFINED ANDROID_PLATFORM)
                string(REGEX MATCH "[0-9]+" _os_api_level ${ANDROID_PLATFORM})
            elseif (DEFINED CMAKE_SYSTEM_VERSION)
                set(_os_api_level ${CMAKE_SYSTEM_VERSION})
            endif ()
            message(STATUS "CMake-Conan: android api level=${_os_api_level}")
            set(${os_api_level} ${_os_api_level} PARENT_SCOPE)
        endif ()
        if (CMAKE_SYSTEM_NAME MATCHES "Darwin|iOS|tvOS|watchOS")
            if (NOT IS_DIRECTORY ${CMAKE_OSX_SYSROOT})
                set(_os_sdk ${CMAKE_OSX_SYSROOT})
            else ()
                if (CMAKE_OSX_SYSROOT MATCHES Simulator)
                    set(apple_platform_suffix simulator)
                else ()
                    set(apple_platform_suffix os)
                endif ()
                if (CMAKE_OSX_SYSROOT MATCHES AppleTV)
                    set(_os_sdk "appletv${apple_platform_suffix}")
                elseif (CMAKE_OSX_SYSROOT MATCHES iPhone)
                    set(_os_sdk "iphone${apple_platform_suffix}")
                elseif (CMAKE_OSX_SYSROOT MATCHES Watch)
                    set(_os_sdk "watch${apple_platform_suffix}")
                endif ()
            endif ()
            if (DEFINED os_sdk)
                message(STATUS "CMake-Conan: cmake_osx_sysroot=${CMAKE_OSX_SYSROOT}")
                set(${os_sdk} ${_os_sdk} PARENT_SCOPE)
            endif ()
            if (DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
                message(STATUS "CMake-Conan: cmake_osx_deployment_target=${CMAKE_OSX_DEPLOYMENT_TARGET}")
                set(${os_version} ${CMAKE_OSX_DEPLOYMENT_TARGET} PARENT_SCOPE)
            endif ()
        endif ()
    endif ()
endfunction()


function(detect_arch arch)
    if (DEFINED CMAKE_OSX_ARCHITECTURES)
        string(REPLACE " " ";" apple_arch_list "${CMAKE_OSX_ARCHITECTURES}")
        list(LENGTH apple_arch_list apple_arch_count)
        if (apple_arch_count GREATER 1)
            message(WARNING "CMake-Conan: Multiple architectures detected, this will only work if Conan recipe(s) produce fat binaries.")
        endif ()
    endif ()
    if (CMAKE_SYSTEM_NAME MATCHES "Darwin|iOS|tvOS|watchOS" AND NOT CMAKE_OSX_ARCHITECTURES STREQUAL "")
        set(host_arch ${CMAKE_OSX_ARCHITECTURES})
    elseif (MSVC)
        set(host_arch ${CMAKE_CXX_COMPILER_ARCHITECTURE_ID})
    else ()
        set(host_arch ${CMAKE_SYSTEM_PROCESSOR})
    endif ()
    if (host_arch MATCHES "aarch64|arm64|ARM64")
        set(_arch armv8)
    elseif (host_arch MATCHES "armv7|armv7-a|armv7l|ARMV7")
        set(_arch armv7)
    elseif (host_arch MATCHES armv7s)
        set(_arch armv7s)
    elseif (host_arch MATCHES "i686|i386|X86")
        set(_arch x86)
    elseif (host_arch MATCHES "AMD64|amd64|x86_64|x64")
        set(_arch x86_64)
    endif ()
    if (EMSCRIPTEN)
        set(_arch wasm)
    endif ()
    message(STATUS "CMake-Conan: cmake_system_processor=${_arch}")
    set(${arch} ${_arch} PARENT_SCOPE)
endfunction()


function(detect_cxx_standard compiler cxx_standard)
    set(${cxx_standard} ${CMAKE_CXX_STANDARD} PARENT_SCOPE)
    if (CMAKE_CXX_EXTENSIONS)
        if (compiler STREQUAL "msvc")
            set(${cxx_standard} "${CMAKE_CXX_STANDARD}" PARENT_SCOPE)
        else ()
            set(${cxx_standard} "gnu${CMAKE_CXX_STANDARD}" PARENT_SCOPE)
        endif ()
    endif ()
endfunction()


macro(detect_gnu_libstdcxx)
    check_cxx_source_compiles("
    #include <cstddef>
    #if !defined(__GLIBCXX__) && !defined(__GLIBCPP__)
    static_assert(false);
    #endif
    int main(){}" _conan_is_gnu_libstdcxx)

    check_cxx_source_compiles("
    #include <string>
    static_assert(sizeof(std::string) != sizeof(void*), \"using libstdc++\");
    int main () {}" _conan_gnu_libstdcxx_is_cxx11_abi)

    set(_conan_gnu_libstdcxx_suffix "")
    if (_conan_gnu_libstdcxx_is_cxx11_abi)
        set(_conan_gnu_libstdcxx_suffix "11")
    endif ()
    unset(_conan_gnu_libstdcxx_is_cxx11_abi)
endmacro()


macro(detect_libcxx)
    check_cxx_source_compiles("
    #include <cstddef>
    #if !defined(_LIBCPP_VERSION)
       static_assert(false);
    #endif
    int main(){}" _conan_is_libcxx)
endmacro()


function(detect_lib_cxx lib_cxx)
    if (CMAKE_SYSTEM_NAME STREQUAL "Android")
        message(STATUS "CMake-Conan: android_stl=${CMAKE_ANDROID_STL_TYPE}")
        set(${lib_cxx} ${CMAKE_ANDROID_STL_TYPE} PARENT_SCOPE)
        return()
    endif ()

    include(CheckCXXSourceCompiles)

    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        detect_gnu_libstdcxx()
        set(${lib_cxx} "libstdc++${_conan_gnu_libstdcxx_suffix}" PARENT_SCOPE)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "AppleClang")
        set(${lib_cxx} "libc++" PARENT_SCOPE)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT CMAKE_SYSTEM_NAME MATCHES "Windows")
        # Check for libc++
        detect_libcxx()
        if (_conan_is_libcxx)
            set(${lib_cxx} "libc++" PARENT_SCOPE)
            return()
        endif ()

        # Check for libstdc++
        detect_gnu_libstdcxx()
        if (_conan_is_gnu_libstdcxx)
            set(${lib_cxx} "libstdc++${_conan_gnu_libstdcxx_suffix}" PARENT_SCOPE)
            return()
        endif ()

    elseif (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
        return()
    else ()
    endif ()
endfunction()


function(detect_compiler compiler compiler_version compiler_runtime compiler_runtime_type)
    if (DEFINED CMAKE_CXX_COMPILER_ID)
        set(_compiler ${CMAKE_CXX_COMPILER_ID})
        set(_compiler_version ${CMAKE_CXX_COMPILER_VERSION})
    else ()
        if (NOT DEFINED CMAKE_C_COMPILER_ID)
            message(FATAL_ERROR "C or C++ compiler not defined")
        endif ()
        set(_compiler ${CMAKE_C_COMPILER_ID})
        set(_compiler_version ${CMAKE_C_COMPILER_VERSION})
    endif ()

    message(STATUS "CMake-Conan: CMake compiler=${_compiler}")
    message(STATUS "CMake-Conan: CMake compiler version=${_compiler_version}")

    if (_compiler MATCHES MSVC)
        set(_compiler "msvc")
        string(SUBSTRING ${MSVC_VERSION} 0 3 _compiler_version)
        if (CMAKE_MSVC_RUNTIME_LIBRARY)
            set(_msvc_runtime_library ${CMAKE_MSVC_RUNTIME_LIBRARY})
        else ()
            set(_msvc_runtime_library MultiThreaded$<$<CONFIG:Debug>:Debug>DLL) # default value documented by CMake
        endif ()

        set(_KNOWN_MSVC_RUNTIME_VALUES "")
        list(APPEND _KNOWN_MSVC_RUNTIME_VALUES MultiThreaded MultiThreadedDLL)
        list(APPEND _KNOWN_MSVC_RUNTIME_VALUES MultiThreadedDebug MultiThreadedDebugDLL)
        list(APPEND _KNOWN_MSVC_RUNTIME_VALUES MultiThreaded$<$<CONFIG:Debug>:Debug> MultiThreaded$<$<CONFIG:Debug>:Debug>DLL)

        if (NOT _msvc_runtime_library IN_LIST _KNOWN_MSVC_RUNTIME_VALUES)
            message(FATAL_ERROR "CMake-Conan: unable to map MSVC runtime: ${_msvc_runtime_library} to Conan settings")
        endif ()

        if (_msvc_runtime_library MATCHES ".*DLL$")
            set(_compiler_runtime "dynamic")
        else ()
            set(_compiler_runtime "static")
        endif ()
        message(STATUS "CMake-Conan: CMake compiler.runtime=${_compiler_runtime}")

        if (NOT _msvc_runtime_library MATCHES "<CONFIG:Debug>:Debug>")
            if (_msvc_runtime_library MATCHES "Debug")
                set(_compiler_runtime_type "Debug")
            else ()
                set(_compiler_runtime_type "Release")
            endif ()
            message(STATUS "CMake-Conan: CMake compiler.runtime_type=${_compiler_runtime_type}")
        endif ()

        unset(_KNOWN_MSVC_RUNTIME_VALUES)

    elseif (_compiler MATCHES AppleClang)
        set(_compiler "apple-clang")
        string(REPLACE "." ";" VERSION_LIST ${_compiler_version})
        list(GET VERSION_LIST 0 _compiler_version)
    elseif (_compiler MATCHES Clang)
        set(_compiler "clang")
        string(REPLACE "." ";" VERSION_LIST ${_compiler_version})
        list(GET VERSION_LIST 0 _compiler_version)
    elseif (_compiler MATCHES GNU)
        set(_compiler "gcc")
        string(REPLACE "." ";" VERSION_LIST ${_compiler_version})
        list(GET VERSION_LIST 0 _compiler_version)
    endif ()

    message(STATUS "CMake-Conan: [settings] compiler=${_compiler}")
    message(STATUS "CMake-Conan: [settings] compiler.version=${_compiler_version}")
    if (_compiler_runtime)
        message(STATUS "CMake-Conan: [settings] compiler.runtime=${_compiler_runtime}")
    endif ()
    if (_compiler_runtime_type)
        message(STATUS "CMake-Conan: [settings] compiler.runtime_type=${_compiler_runtime_type}")
    endif ()

    set(${compiler} ${_compiler} PARENT_SCOPE)
    set(${compiler_version} ${_compiler_version} PARENT_SCOPE)
    set(${compiler_runtime} ${_compiler_runtime} PARENT_SCOPE)
    set(${compiler_runtime_type} ${_compiler_runtime_type} PARENT_SCOPE)
endfunction()


function(detect_build_type build_type)
    get_property(multiconfig_generator GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if (NOT multiconfig_generator)
        set(${build_type} ${CMAKE_BUILD_TYPE} PARENT_SCOPE)
    endif ()
endfunction()


macro(set_conan_compiler_if_appleclang lang command output_variable)
    if (CMAKE_${lang}_COMPILER_ID STREQUAL "AppleClang")
        execute_process(COMMAND xcrun --find ${command}
                OUTPUT_VARIABLE _xcrun_out OUTPUT_STRIP_TRAILING_WHITESPACE)
        cmake_path(GET _xcrun_out PARENT_PATH _xcrun_toolchain_path)
        cmake_path(GET CMAKE_${lang}_COMPILER PARENT_PATH _compiler_parent_path)
        if ("${_xcrun_toolchain_path}" STREQUAL "${_compiler_parent_path}")
            set(${output_variable} "")
        endif ()
        unset(_xcrun_out)
        unset(_xcrun_toolchain_path)
        unset(_compiler_parent_path)
    endif ()
endmacro()


macro(append_compiler_executables_configuration)
    set(_conan_c_compiler "")
    set(_conan_cpp_compiler "")
    set(_conan_rc_compiler "")
    set(_conan_compilers_list "")
    if (CMAKE_C_COMPILER)
        set(_conan_c_compiler "\"c\":\"${CMAKE_C_COMPILER}\"")
        set_conan_compiler_if_appleclang(C cc _conan_c_compiler)
        list(APPEND _conan_compilers_list ${_conan_c_compiler})
    else ()
        message(WARNING "CMake-Conan: The C compiler is not defined. " "Please define CMAKE_C_COMPILER or enable the C language.")
    endif ()
    if (CMAKE_CXX_COMPILER)
        set(_conan_cpp_compiler "\"cpp\":\"${CMAKE_CXX_COMPILER}\"")
        set_conan_compiler_if_appleclang(CXX c++ _conan_cpp_compiler)
        list(APPEND _conan_compilers_list ${_conan_cpp_compiler})
    else ()
        message(WARNING "CMake-Conan: The C++ compiler is not defined. "
                "Please define CMAKE_CXX_COMPILER or enable the C++ language.")
    endif ()
    if (CMAKE_RC_COMPILER)
        set(_conan_rc_compiler "\"rc\":\"${CMAKE_RC_COMPILER}\"")
        list(APPEND _conan_compilers_list ${_conan_rc_compiler})
    endif ()
    if (NOT "x${_conan_compilers_list}" STREQUAL "x")
        if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            string(REPLACE ";" "," _conan_compilers_list "${_conan_compilers_list}")
            string(APPEND profile "tools.build:compiler_executables={${_conan_compilers_list}}\n")
        endif ()
    endif ()
    unset(_conan_c_compiler)
    unset(_conan_cpp_compiler)
    unset(_conan_rc_compiler)
    unset(_conan_compilers_list)
endmacro()


function(detect_host_profile output_file)
    detect_os(os os_api_level os_sdk os_subsystem os_version)
    detect_arch(arch)
    detect_compiler(compiler compiler_version compiler_runtime compiler_runtime_type)
    detect_cxx_standard(${compiler} compiler_cppstd)
    detect_lib_cxx(compiler_libcxx)
    detect_build_type(build_type)

    set(profile "")
    string(APPEND profile "[settings]\n")
    if (arch)
        string(APPEND profile arch=${arch} "\n")
    endif ()
    if (os)
        string(APPEND profile os=${os} "\n")
    endif ()
    if (os_api_level)
        string(APPEND profile os.api_level=${os_api_level} "\n")
    endif ()
    if (os_version)
        string(APPEND profile os.version=${os_version} "\n")
    endif ()
    if (os_sdk)
        string(APPEND profile os.sdk=${os_sdk} "\n")
    endif ()
    if (os_subsystem)
        string(APPEND profile os.subsystem=${os_subsystem} "\n")
    endif ()
    if (compiler)
        string(APPEND profile compiler=${compiler} "\n")
    endif ()
    if (compiler_version)
        string(APPEND profile compiler.version=${compiler_version} "\n")
    endif ()
    if (compiler_runtime)
        string(APPEND profile compiler.runtime=${compiler_runtime} "\n")
    endif ()
    if (compiler_runtime_type)
        string(APPEND profile compiler.runtime_type=${compiler_runtime_type} "\n")
    endif ()
    if (compiler_cppstd)
        string(APPEND profile compiler.cppstd=${compiler_cppstd} "\n")
    endif ()
    if (compiler_libcxx)
        string(APPEND profile compiler.libcxx=${compiler_libcxx} "\n")
    endif ()
    if (build_type)
        string(APPEND profile "build_type=${build_type}\n")
    endif ()

    if (NOT DEFINED output_file)
        set(file_name "${CMAKE_BINARY_DIR}/profile")
    else ()
        set(file_name ${output_file})
    endif ()

    string(APPEND profile "[conf]\n")
    string(APPEND profile "tools.cmake.cmaketoolchain:generator=${CMAKE_GENERATOR}\n")

    append_compiler_executables_configuration()

    if (os STREQUAL "Android")
        string(APPEND profile "tools.android:ndk_path=${CMAKE_ANDROID_NDK}\n")
    endif ()

    message(STATUS "CMake-Conan: Creating profile ${file_name}")
    file(WRITE ${file_name} ${profile})
    message(STATUS "CMake-Conan: Profile: \n${profile}")
endfunction()


function(conan_profile_detect_default)
    message(STATUS "CMake-Conan: Checking if a default profile exists")
    execute_process(COMMAND ${CONAN_COMMAND} profile path default
            RESULT_VARIABLE return_code
            OUTPUT_VARIABLE conan_stdout
            ERROR_VARIABLE conan_stderr
            ECHO_ERROR_VARIABLE
            ECHO_OUTPUT_VARIABLE
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    if (NOT ${return_code} EQUAL "0")
        message(STATUS "CMake-Conan: The default profile doesn't exist, detecting it.")
        execute_process(COMMAND ${CONAN_COMMAND} profile detect
                RESULT_VARIABLE return_code
                OUTPUT_VARIABLE conan_stdout
                ERROR_VARIABLE conan_stderr
                ECHO_ERROR_VARIABLE
                ECHO_OUTPUT_VARIABLE
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    endif ()
endfunction()


function(conan_install)
    set(conan_output_folder ${CMAKE_BINARY_DIR}/conan)
    set(conan_args -of=${conan_output_folder})
    message(STATUS "CMake-Conan: conan install ${CMAKE_SOURCE_DIR} ${conan_args} ${ARGN}")


    if (DEFINED PATH_TO_CMAKE_BIN)
        set(old_path $ENV{PATH})
        set(ENV{PATH} "$ENV{PATH}:${PATH_TO_CMAKE_BIN}")
    endif ()

    execute_process(COMMAND ${CONAN_COMMAND} install ${CMAKE_SOURCE_DIR} ${conan_args} ${ARGN} --format=json
            RESULT_VARIABLE return_code
            OUTPUT_VARIABLE conan_stdout
            ERROR_VARIABLE conan_stderr
            ECHO_ERROR_VARIABLE
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

    if (DEFINED PATH_TO_CMAKE_BIN)
        set(ENV{PATH} "${old_path}")
    endif ()

    if (NOT "${return_code}" STREQUAL "0")
        message(FATAL_ERROR "Conan install failed='${return_code}'")
    endif ()

    string(JSON conan_generators_folder GET "${conan_stdout}" graph nodes 0 generators_folder)
    cmake_path(CONVERT ${conan_generators_folder} TO_CMAKE_PATH_LIST conan_generators_folder)

    message(STATUS "CMake-Conan: CONAN_GENERATORS_FOLDER=${conan_generators_folder}")
    set_property(GLOBAL PROPERTY CONAN_GENERATORS_FOLDER "${conan_generators_folder}")
    string(JSON conanfile GET "${conan_stdout}" graph nodes 0 label)
    message(STATUS "CMake-Conan: CONANFILE=${CMAKE_SOURCE_DIR}/${conanfile}")
    set_property(DIRECTORY ${CMAKE_SOURCE_DIR} APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/${conanfile}")
    set_property(GLOBAL PROPERTY CONAN_INSTALL_SUCCESS TRUE)

endfunction()


function(conan_get_version conan_command conan_current_version)
    execute_process(
            COMMAND ${conan_command} --version
            OUTPUT_VARIABLE conan_output
            RESULT_VARIABLE conan_result
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if (conan_result)
        message(FATAL_ERROR "CMake-Conan: Error when trying to run Conan")
    endif ()

    string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" conan_version ${conan_output})
    set(${conan_current_version} ${conan_version} PARENT_SCOPE)
endfunction()


function(conan_version_check)
    set(options)
    set(one_value_args MINIMUM CURRENT)
    set(multi_value_args)
    cmake_parse_arguments(conan_version_check
            "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if (NOT conan_version_check_MINIMUM)
        message(FATAL_ERROR "CMake-Conan: Required parameter MINIMUM not set!")
    endif ()
    if (NOT conan_version_check_CURRENT)
        message(FATAL_ERROR "CMake-Conan: Required parameter CURRENT not set!")
    endif ()

    if (conan_version_check_CURRENT VERSION_LESS conan_version_check_MINIMUM)
        message(FATAL_ERROR "CMake-Conan: Conan version must be ${conan_version_check_MINIMUM} or later")
    endif ()
endfunction()


macro(construct_profile_argument argument_variable profile_list)
    set(${argument_variable} "")
    if ("${profile_list}" STREQUAL "CONAN_HOST_PROFILE")
        set(_arg_flag "--profile:host=")
    elseif ("${profile_list}" STREQUAL "CONAN_BUILD_PROFILE")
        set(_arg_flag "--profile:build=")
    endif ()

    set(_profile_list "${${profile_list}}")
    list(TRANSFORM _profile_list REPLACE "auto-cmake" "${CMAKE_BINARY_DIR}/conan_host_profile")
    list(TRANSFORM _profile_list PREPEND ${_arg_flag})
    set(${argument_variable} ${_profile_list})

    unset(_arg_flag)
    unset(_profile_list)
endmacro()


macro(conan_provide_dependency method package_name)
    set_property(GLOBAL PROPERTY CONAN_PROVIDE_DEPENDENCY_INVOKED TRUE)
    get_property(_conan_install_success GLOBAL PROPERTY CONAN_INSTALL_SUCCESS)
    if (NOT _conan_install_success)
        find_program(CONAN_COMMAND "conan" REQUIRED)
        conan_get_version("${CONAN_COMMAND}" CONAN_CURRENT_VERSION)
        conan_version_check(MINIMUM ${CONAN_MINIMUM_VERSION} CURRENT ${CONAN_CURRENT_VERSION})
        message(STATUS "CMake-Conan: first find_package() found. Installing dependencies with Conan")
        if ("default" IN_LIST CONAN_HOST_PROFILE OR "default" IN_LIST CONAN_BUILD_PROFILE)
            conan_profile_detect_default()
        endif ()
        if ("auto-cmake" IN_LIST CONAN_HOST_PROFILE)
            detect_host_profile(${CMAKE_BINARY_DIR}/conan_host_profile)
        endif ()
        construct_profile_argument(_host_profile_flags CONAN_HOST_PROFILE)
        construct_profile_argument(_build_profile_flags CONAN_BUILD_PROFILE)
        if (EXISTS "${CMAKE_SOURCE_DIR}/conanfile.py")
            file(READ "${CMAKE_SOURCE_DIR}/conanfile.py" outfile)
            if (NOT "${outfile}" MATCHES ".*CMakeConfigDeps.*")
                message(WARNING "Cmake-conan: CMakeConfigDeps generator was not defined in the conanfile")
            endif ()
        elseif (EXISTS "${CMAKE_SOURCE_DIR}/conanfile.txt")
            file(READ "${CMAKE_SOURCE_DIR}/conanfile.txt" outfile)
            if (NOT "${outfile}" MATCHES ".*CMakeConfigDeps.*")
                message(WARNING "Cmake-conan: CMakeConfigDeps generator was not defined in the conanfile")
            endif ()
        endif ()

        get_property(_multiconfig_generator GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

        if (DEFINED CONAN_INSTALL_BUILD_CONFIGURATIONS)
            set(_build_configs "${CONAN_INSTALL_BUILD_CONFIGURATIONS}")
            list(LENGTH _build_configs _build_configs_length)
            if (NOT _multiconfig_generator AND _build_configs_length GREATER 1)
                message(FATAL_ERROR "cmake-conan: when using a single-config CMake generator, "
                        "please only specify a single configuration in CONAN_INSTALL_BUILD_CONFIGURATIONS")
            endif ()
            unset(_build_configs_length)
        else ()
            if (_multiconfig_generator)
                set(_build_configs Release Debug)
            else ()
                set(_build_configs ${CMAKE_BUILD_TYPE})
            endif ()

        endif ()

        list(JOIN _build_configs ", " _build_configs_msg)
        message(STATUS "CMake-Conan: Installing configuration(s): ${_build_configs_msg}")
        foreach (_build_config IN LISTS _build_configs)
            set(_self_build_config "")
            if (NOT _multiconfig_generator AND NOT _build_config STREQUAL "${CMAKE_BUILD_TYPE}")
                set(_self_build_config -s &:build_type=${CMAKE_BUILD_TYPE})
            endif ()
            conan_install(${_host_profile_flags} ${_build_profile_flags} -s build_type=${_build_config} ${_self_build_config} ${CONAN_INSTALL_ARGS})
        endforeach ()

        get_property(_conan_generators_folder GLOBAL PROPERTY CONAN_GENERATORS_FOLDER)
        if (EXISTS "${_conan_generators_folder}/conan_cmakedeps_paths.cmake")
            message(STATUS "CMake-Conan: Loading conan_cmakedeps_paths.cmake file")
            include(${_conan_generators_folder}/conan_cmakedeps_paths.cmake)
        endif ()

        unset(_self_build_config)
        unset(_multiconfig_generator)
        unset(_build_configs)
        unset(_build_configs_msg)
        unset(_host_profile_flags)
        unset(_build_profile_flags)
        unset(_conan_install_success)
    else ()
        message(STATUS "CMake-Conan: find_package(${ARGV1}) found, 'conan install' already ran")
        unset(_conan_install_success)
    endif ()

    get_property(_conan_generators_folder GLOBAL PROPERTY CONAN_GENERATORS_FOLDER)

    set(_find_args_${package_name} "${ARGN}")
    list(REMOVE_ITEM _find_args_${package_name} "REQUIRED")
    if (NOT "MODULE" IN_LIST _find_args_${package_name})
        find_package(${package_name} ${_find_args_${package_name}} BYPASS_PROVIDER PATHS "${_conan_generators_folder}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
        unset(_find_args_${package_name})
    endif ()

    if (NOT ${package_name}_FOUND)
        list(FIND CMAKE_MODULE_PATH "${_conan_generators_folder}" _index)
        if (_index EQUAL -1)
            list(PREPEND CMAKE_MODULE_PATH "${_conan_generators_folder}")
        endif ()
        unset(_index)
        find_package(${package_name} ${ARGN} BYPASS_PROVIDER)
        list(REMOVE_ITEM CMAKE_MODULE_PATH "${_conan_generators_folder}")
    endif ()
endmacro()


cmake_language(
        SET_DEPENDENCY_PROVIDER conan_provide_dependency
        SUPPORTED_METHODS FIND_PACKAGE
)


macro(conan_provide_dependency_check)
    set(_conan_provide_dependency_invoked FALSE)
    get_property(_conan_provide_dependency_invoked GLOBAL PROPERTY CONAN_PROVIDE_DEPENDENCY_INVOKED)
    if (NOT _conan_provide_dependency_invoked)
        message(WARNING "Conan is correctly configured as dependency provider, "
                "but Conan has not been invoked. Please add at least one "
                "call to `find_package()`.")
        if (DEFINED CONAN_COMMAND)
            set(_conan_command ${CONAN_COMMAND})
            unset(_conan_command)
        endif ()
    endif ()
    unset(_conan_provide_dependency_invoked)
endmacro()


cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL conan_provide_dependency_check)

set(CONAN_HOST_PROFILE "default;auto-cmake" CACHE STRING "Conan host profile")
set(CONAN_BUILD_PROFILE "default" CACHE STRING "Conan build profile")
set(CONAN_INSTALL_ARGS "--build=missing" CACHE STRING "Command line arguments for conan install")

find_program(_cmake_program NAMES cmake NO_PACKAGE_ROOT_PATH NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH NO_CMAKE_SYSTEM_PATH NO_CMAKE_FIND_ROOT_PATH)
if (NOT _cmake_program)
    get_filename_component(PATH_TO_CMAKE_BIN "${CMAKE_COMMAND}" DIRECTORY)
    set(PATH_TO_CMAKE_BIN "${PATH_TO_CMAKE_BIN}" CACHE INTERNAL "Path where the CMake executable is")
endif ()

cmake_policy(POP)
