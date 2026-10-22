"""把五个静态库与公开头文件打成 Conan 库包 asyngyanis/<版本>。

用法：conan create packaging/conan --build=missing
消费：requires asyngyanis/<版本>，CMakeDeps + CMakeToolchain 生成后 find_package(AsynGyanis)。
只出静态库；共享形态走仓库自身的 -DBUILD_SHARED_LIBS=ON。
版本取自根 CMakeLists 的 project(VERSION)，依赖清单须与 conandata.yml 同解。
与根 conanfile.py 分工：那份是开发配方（application 角色，可跳过 MySQL），这份只管分发库本体。
"""

import os
import re

from conan import ConanFile
from conan.errors import ConanException
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy

# 库本体必需依赖：openssl 管 TLS 与摘要，zlib/zstd/brotli 管压缩协商，sqlite3/hiredis 管 Database。
# 须与 src/*/CMakeLists.txt 的 find_package(... REQUIRED) 及 conandata.yml 同解：漏一项就在
# configure 阶段失败，check-dependency-advisories.py 机检。HTTP/3 与 QPACK 自研，故无 h3 依赖。
BASE_REQUIREMENTS = [
    "zlib/1.3.1",
    "zstd/1.5.7",
    "brotli/1.1.0",
    "openssl/3.6.5",
    "sqlite3/3.51.3",
    "hiredis/1.3.0",
]

# Base 的原生格式接口：JSON 头文件直接出现在公开头 ConfigValue.h 里，消费方需要它的包含目录；
# YAML 只在实现里使用，但 Base 是静态库，符号要由消费方在链接时一并向库解析。
JSON_REQUIREMENT = "nlohmann_json/3.12.0"
YAML_REQUIREMENT = "yaml-cpp/0.9.0"

# MySQL 驱动是可选依赖：ConanCenter 上只有源码包，装它要现场编译十几分钟。
# 不开时库照常构建，MySQL 驱动退化为报错桩（-DDATABASE_WITH_MYSQL=OFF）。
MYSQL_REQUIREMENT = "libmysqlclient/8.1.0"

# PostgreSQL 驱动同为可选依赖，不开则退化为报错桩（-DDATABASE_WITH_POSTGRES=OFF）。
# ConanCenter 上各平台有预编译的 Release 二进制，只有 Debug 档要源码构建（meson，约一分钟）。
POSTGRES_REQUIREMENT = "libpq/17.11"


class AsynGyanisLibrary(ConanFile):
    name = "asyngyanis"
    description = ("C++23 协程服务端框架：epoll/完成端口（可选 io_uring）事件循环、HTTP/1.1 与 HTTP/2 与 "
                   "HTTP/3(自研 QUIC)、WebSocket、ACME 证书自动化、SqlSugar 风格 ORM")
    package_type = "static-library"
    license = "MIT"
    settings = "os", "compiler", "build_type", "arch"

    options = {"with_mysql": [True, False], "with_postgres": [True, False]}
    default_options = {"with_mysql": False, "with_postgres": False}

    def set_version(self):
        """从根 CMakeLists 的 project(AsynGyanis VERSION x.y.z) 读版本号，单一真源；读不到就拒。"""
        repositoryRoot = os.path.abspath(os.path.join(self.recipe_folder, "..", ".."))
        with open(os.path.join(repositoryRoot, "CMakeLists.txt"), encoding="utf-8") as source:
            match = re.search(r"project\(\s*AsynGyanis\s+VERSION\s+(\d+\.\d+\.\d+)", source.read())
        if not match:
            raise ConanException("根 CMakeLists.txt 里找不到 project(AsynGyanis VERSION x.y.z)，包版本无从取得")
        self.version = match.group(1)

    def export_sources(self):
        # 源就在仓库里，导出时从仓库根整棵快照：包内容与仓库版本严格对应，配方不复制任何源。
        # 只导出构建必需的 CMakeLists.txt、src/**、cmake/**；测试与示例由 generate() 的开关关掉。
        # LICENSE 必须一起导出：包在别处构建时 self.recipe_folder 指向缓存，仓库根已不存在。
        repository_root = os.path.abspath(os.path.join(self.recipe_folder, "..", ".."))
        copy(self, "CMakeLists.txt", src=repository_root, dst=self.export_sources_folder)
        # 目录必须用 ** 递归匹配：不带通配符的 "src" 只会去找一个叫 src 的文件。
        copy(self, "src/**", src=repository_root, dst=self.export_sources_folder)
        copy(self, "cmake/**", src=repository_root, dst=self.export_sources_folder)
        copy(self, "LICENSE", src=repository_root, dst=self.export_sources_folder)

    def layout(self):
        cmake_layout(self)

    def requirements(self):
        for requirement in BASE_REQUIREMENTS:
            self.requires(requirement)
        self.requires(JSON_REQUIREMENT, transitive_headers=True)
        self.requires(YAML_REQUIREMENT, transitive_libs=True)
        if self.options.with_mysql:
            self.requires(MYSQL_REQUIREMENT)
        if self.options.with_postgres:
            self.requires(POSTGRES_REQUIREMENT)

    def generate(self):
        CMakeDeps(self).generate()

        toolchain = CMakeToolchain(self)
        toolchain.user_presets_path = False
        # C++ 标准只在根 CMakeLists.txt 定义：那里写的普通变量会盖掉这里的缓存值，不再重复传。
        # 开发者产物一律不构建：消费方只需要库与它的安装规则。
        toolchain.cache_variables["ASYN_BUILD_TESTS"] = "OFF"
        toolchain.cache_variables["ASYN_BUILD_SAMPLES"] = "OFF"
        toolchain.cache_variables["DATABASE_WITH_MYSQL"] = "ON" if self.options.with_mysql else "OFF"
        toolchain.cache_variables["DATABASE_WITH_POSTGRES"] = "ON" if self.options.with_postgres else "OFF"
        toolchain.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        # 只构建 install 目标：它依赖五个模块库，其余什么都不碰
        cmake.build(target="install")

    def package(self):
        # 库本体已在 build() 里按 install 规则落进包目录（CMakeToolchain 把前缀指向包）。
        # 这里只补许可证文本：license = "MIT" 是元数据，包里没有文件等于对外说 MIT 却不交付。
        copy(self, "LICENSE", src=self.source_folder, dst=os.path.join(self.package_folder, "licenses"))

    def _stacktraceSupportMarker(self) -> dict:
        """读构建期 std::stacktrace 探测落下的标记文件，缺失就拒打包。

        真源是 src/Base/CMakeLists.txt 的 check_cxx_source_compiles：宏是 PUBLIC 的，
        StackTrace.h 按它换 CapturedStackTrace 的类型，在配方里复述工具链矩阵必然分叉。
        """
        markerName = "AsynGyanisStacktraceSupport.txt"
        markerPath = None
        # 标记落在 <datarootdir>/asyngyanis 下，res/ 还是 share/ 由 GNUInstallDirs 与生成器决定，
        # 因此按名字找、不拼死路径；找不到就拒打包。
        for root, _, fileNames in os.walk(self.package_folder):
            if markerName in fileNames:
                markerPath = os.path.join(root, markerName)
                break
        if markerPath is None:
            raise ConanException(
                f"包里找不到 {markerName}：那份 std::stacktrace 探测结果没能随包交付，"
                "配方无从判断消费方要不要 ASYN_HAS_STACKTRACE（宁可拒，也不要按猜出来的矩阵发一个编译宏）")
        fields = {}
        with open(markerPath, encoding="utf-8") as source:
            for line in source.read().splitlines():
                key, separator, value = line.partition("=")
                if separator:
                    fields[key.strip()] = value.strip()
        if fields.get("has-stacktrace") not in ("0", "1"):
            raise ConanException(f"包里的 std::stacktrace 标记读不出取值：{fields}")
        return fields

    def _installedDriverDefines(self) -> list:
        """从随包交付的能力清单读哪几个驱动编的是真实现，真源是 src/Database/CMakeLists.txt。

        选项开着而客户端库装不上时 CMake 退化为报错桩，此时 PUBLIC 宏 DATABASE_HAS_<名> 不存在；
        按选项复述会发出库体里没有的宏，静态链接不报错，错的是两边对同一个类的布局。缺清单就拒。
        """
        featuresPath = os.path.join(self.package_folder, "include", "AsynGyanisFeatures.h")
        if not os.path.exists(featuresPath):
            raise ConanException(
                f"包里找不到 {featuresPath}：能力清单没随包交付，配方无从判断三个驱动宏要不要发给消费方")
        with open(featuresPath, encoding="utf-8") as source:
            text = source.read()
        return [f"DATABASE_HAS_{driver}=1" for driver in ("MYSQL", "POSTGRES", "REDIS")
                if re.search(rf"^#define ASYN_HAS_{driver}_DRIVER 1$", text, re.MULTILINE)]

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "AsynGyanis")

        # 组件依赖与 src/*/CMakeLists.txt 的 target_link_libraries 一一对应：组件名指内部目标，
        # <包名>::<包名> 指外部依赖。静态库不把外部依赖带给最终可执行文件，少一个就是链接期
        # 「无法解析的外部符号 deflate/SSL_new/...」。
        platform = self.cpp_info.components["platform"]
        platform.libs = ["Platform"]
        platform.set_property("cmake_target_name", "AsynGyanis::Platform")
        if self.settings.os == "Windows":
            # Winsock 只在 Windows 需要（Linux 由 libc 内建），Linux 侧补 pthread。清单须与
            # src/Platform/CMakeLists.txt 的 target_link_libraries 逐项一致：少一项就是链接期
            # 未解析符号。iphlpapi 不能漏，NetworkInterface.h 是公开头且直接 include <iphlpapi.h>。
            platform.system_libs = ["ws2_32", "Mswsock", "iphlpapi"]
        else:
            platform.system_libs = ["pthread"]

        base = self.cpp_info.components["base"]
        base.libs = ["Base"]
        base.requires = ["platform", "nlohmann_json::nlohmann_json", "yaml-cpp::yaml-cpp"]
        base.set_property("cmake_target_name", "AsynGyanis::Base")

        # std::stacktrace 能力在 CMake 是 PUBLIC：探测通过后挂 ASYN_HAS_STACKTRACE=1 与 stdc++exp。
        # 包这一侧必须同样声明：少 defines 会让消费方按空替身编译、与 libBase.a 布局不一致；
        # 少 system_libs 则链接期 undefined reference to __glibcxx_backtrace_simple。
        stacktraceSupport = self._stacktraceSupportMarker()
        if stacktraceSupport["has-stacktrace"] == "1":
            base.defines = ["ASYN_HAS_STACKTRACE=1"]
            extraSystemLibs = [entry for entry in stacktraceSupport.get("link-libs", "").split(";") if entry]
            if extraSystemLibs:
                base.system_libs = list(base.system_libs) + extraSystemLibs

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
        # 三个驱动的 PUBLIC 编译宏也要发给消费方：CMakeDeps 只认 cpp_info 的编译定义，
        # install(EXPORT) 的 INTERFACE_COMPILE_DEFINITIONS 不走这条路。少了它，消费方按桩形状
        # 编译而 libDatabase.a 里是真驱动——链接不报错，错的是两边的类型布局。
        database.defines = list(database.defines) + self._installedDriverDefines()
        if self.options.with_mysql:
            database.requires.append("libmysqlclient::libmysqlclient")
        if self.options.with_postgres:
            # 组件名是 libpq 包里的 pq（其 cmake_target_name 指向 PostgreSQL::PostgreSQL）；
            # 写成 libpq::libpq 会在 CMakeDeps 生成期报找不到该组件。
            database.requires.append("libpq::pq")

        # 公开头是 UTF-8（含中文注释），MSVC 默认按系统代码页解码会把注释放进下一行，报假语法错误。
        # CMake 里它是 INTERFACE 选项，但 CMakeDeps 只认 cpp_info 的编译选项，因此这里再声明一次；
        # 少它，消费方只要包含我们的头就会踩同一个坑。
        if self.settings.compiler == "msvc":
            for component in self.cpp_info.components.values():
                component.cxxflags = ["/utf-8"]
