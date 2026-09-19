"""
@file conanfile.py
@brief 把本仓库的五个静态库与公开头文件打成 Conan 库包（asyngyanis/<版本>）
@author Gyanis
@date 2026-09-13
@version 1.0.0
@copyright Copyright (c) . All rights reserved.

用法（在仓库根目录执行，源在导出时从仓库根整棵快照进包）：

    conan create packaging/conan --build=missing

消费方在 conanfile 里 requires("asyngyanis/1.0.0")，用 CMakeDeps + CMakeToolchain 生成器，
然后 find_package(AsynGyanis) 并按目标取用：AsynGyanis::Platform / Base / Core / Net / Database
。各组件之间的依赖关系由本文件的 cpp_info.components
如实声明，因此只需链顶层用到的那个目标。

与仓库根 conanfile.py 的分工：那份是**开发用**配方（application 角色，只负责为本地构建与 CI
装依赖，并放行 MySQL 依赖的可选跳过）；这份是**分发用**配方，只装库本体必需的四项依赖
（zlib / openssl / sqlite3 / hiredis，可选 libmysqlclient），不引入 GTest——测试与示例由
ASYN_BUILD_TESTS / ASYN_BUILD_SAMPLES 两个开关在配置阶段关掉。

版本号与根 CMakeLists 的 project(VERSION ...) 一致，由 scripts/check-release-version.py 守着。
"""

import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy

# 库本体必需的外部依赖：TLS 与摘要走 openssl，HTTP 响应压缩与 permessage-deflate 走 zlib，
# 压缩协商里还有 zstd 与 brotli（gzip/deflate/zstd/br 四选一的其余两项），
# Database 模块的 SQLite 与 Redis 驱动分别走 sqlite3 与 hiredis。
# **这份清单必须与 src/*/CMakeLists.txt 里的 find_package(... REQUIRED) 对齐**：漏一项，
# 包就在 configure 阶段直接失败（本清单曾漏掉 zstd/brotli/nghttp3，conan create 从来没跑到过）
# HTTP/3 的帧与 QPACK 已全部自研，nghttp3 只在仓库自带的测试里当跨实现裁判，
# 因此它**不进**库包：包消费方不会拿到一条用不上的 find_dependency。
BASE_REQUIREMENTS = [
    "zlib/1.3.1",
    "zstd/1.5.7",
    "brotli/1.1.0",
    "openssl/3.6.2",
    "sqlite3/3.51.3",
    "hiredis/1.3.0",
]

# Base 的原生格式接口：JSON 头文件直接出现在公开头（ConfigValue.h）里，消费方需要它的包含目录；
# YAML 库只在实现里使用，但 Base 是静态库，它的符号要由消费方在链接时一并向库解析
JSON_REQUIREMENT = "nlohmann_json/3.12.0"
YAML_REQUIREMENT = "yaml-cpp/0.9.0"

# MySQL 驱动是可选依赖：ConanCenter 上只有源码包，装它要现场编译十几分钟。
# 不开这个选项时库照常构建，MySQL 驱动退化为报错桩（-DDATABASE_WITH_MYSQL=OFF）
MYSQL_REQUIREMENT = "libmysqlclient/8.1.0"


class AsynGyanisLibrary(ConanFile):
    name = "asyngyanis"
    version = "1.0.0"
    description = "工业级 C++23 协程服务端框架（epoll/wepoll 传输层 + HTTP/1.1 + HTTP/2 + WebSocket）"
    package_type = "static-library"
    license = "Proprietary"
    settings = "os", "compiler", "build_type", "arch"

    options = {"with_mysql": [True, False]}
    default_options = {"with_mysql": False}

    def export_sources(self):
        # 源就在本仓库里，导出时从仓库根整棵快照：包内容因此与仓库版本严格对应，不需要在
        # 配方里复制任何一份源。四项都是构建库本体必需的：CMakeLists.txt（顶层入口）、
        # src/**（模块源）、cmake/**（构建助手）、third_party/**（vendor 进来的 ngtcp2 与
        # 它的 OpenSSL QUIC 探针配置——顶层 CMakeLists 无条件 add_subdirectory(third_party)，
        # 漏了它配置期就失败，而这条路径没有任何 CI 作业跑到，所以只能在改配方时盯住）。
        # 测试/示例/基准目录不进来：它们由下面的两个开关在配置阶段关掉，缺目录也不会被碰到
        repository_root = os.path.abspath(os.path.join(self.recipe_folder, "..", ".."))
        copy(self, "CMakeLists.txt", src=repository_root, dst=self.export_sources_folder)
        # 目录必须用 ** 递归匹配：不带通配符的 "src" 只会去找一个叫 src 的文件，目录匹配不上
        copy(self, "src/**", src=repository_root, dst=self.export_sources_folder)
        copy(self, "cmake/**", src=repository_root, dst=self.export_sources_folder)
        copy(self, "third_party/**", src=repository_root, dst=self.export_sources_folder)

    def layout(self):
        cmake_layout(self)

    def requirements(self):
        for requirement in BASE_REQUIREMENTS:
            self.requires(requirement)
        self.requires(JSON_REQUIREMENT, transitive_headers=True)
        self.requires(YAML_REQUIREMENT, transitive_libs=True)
        if self.options.with_mysql:
            self.requires(MYSQL_REQUIREMENT)

    def generate(self):
        CMakeDeps(self).generate()

        toolchain = CMakeToolchain(self)
        toolchain.user_presets_path = False
        toolchain.cache_variables["CMAKE_CXX_STANDARD"] = "20"
        toolchain.cache_variables["CMAKE_CXX_STANDARD_REQUIRED"] = "ON"
        toolchain.cache_variables["CMAKE_CXX_EXTENSIONS"] = "OFF"
        # 开发者产物一律不构建：消费方只需要库与它的安装规则
        toolchain.cache_variables["ASYN_BUILD_TESTS"] = "OFF"
        toolchain.cache_variables["ASYN_BUILD_SAMPLES"] = "OFF"
        toolchain.cache_variables["DATABASE_WITH_MYSQL"] = "ON" if self.options.with_mysql else "OFF"
        toolchain.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        # 只构建 install 目标：它依赖五个模块库，其余什么都不碰
        cmake.build(target="install")

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "AsynGyanis")

        # 组件的依赖关系与 src/*/CMakeLists.txt 的 target_link_libraries 一一对应：组件之间
        # 用组件名，外部依赖用 <包名>::<包名> 的形式引用，CMakeDeps 据此把它们的导入目标接到
        # 我们的目标上。静态库不会把外部依赖自动带给最终可执行文件，因此这里必须逐个声明，
        # 少一个的后果就是消费方链接时报「无法解析的外部符号 deflate/SSL_new/...」
        platform = self.cpp_info.components["platform"]
        platform.libs = ["Platform"]
        platform.set_property("cmake_target_name", "AsynGyanis::Platform")
        if self.settings.os == "Windows":
            # Winsock 只在 Windows 需要（Linux 侧由 libc 内建提供）；Linux 侧补 pthread
            platform.system_libs = ["ws2_32", "Mswsock"]
        else:
            platform.system_libs = ["pthread"]

        base = self.cpp_info.components["base"]
        base.libs = ["Base"]
        base.requires = ["platform", "nlohmann_json::nlohmann_json", "yaml-cpp::yaml-cpp"]
        base.set_property("cmake_target_name", "AsynGyanis::Base")

        core = self.cpp_info.components["core"]
        core.libs = ["Core"]
        core.requires = ["base", "openssl::openssl"]
        core.set_property("cmake_target_name", "AsynGyanis::Core")

        net = self.cpp_info.components["net"]
        net.libs = ["Net"]
        # 压缩三项在 Net 的 CMake 里是 PRIVATE 链接，但静态库不会把它们带给最终
        # 可执行文件：这里必须逐个声明，少一个就是消费方链接期「无法解析的外部符号」
        net.requires = ["core", "zlib::zlib", "zstd::zstdlib", "brotli::brotli"]
        net.set_property("cmake_target_name", "AsynGyanis::Net")

        database = self.cpp_info.components["database"]
        database.libs = ["Database"]
        database.requires = ["core", "sqlite3::sqlite3", "hiredis::hiredis"]
        database.set_property("cmake_target_name", "AsynGyanis::Database")
        if self.options.with_mysql:
            database.requires.append("libmysqlclient::libmysqlclient")

        # 公开头文件是 UTF-8（含中文注释），而 MSVC 默认按系统代码页解码源码：在 GBK 环境里
        # 会把注释字节吃进下一行，报出「找不到标识符」这类假语法错误。库在 CMake 里把它作为
        # INTERFACE 选项传给使用方，但 CMakeDeps 只认 cpp_info 里声明的编译选项，因此这里必须
        # 再声明一次——少了它，消费方一旦在自己的源文件里写中文（或仅仅是包含我们的头文件）
        # 就会踩到同一个坑
        if self.settings.compiler == "msvc":
            for component in self.cpp_info.components.values():
                component.cxxflags = ["/utf-8"]
