import os

from conan import ConanFile
from conan.tools.cmake import cmake_layout, CMakeToolchain

# 环境变量开关：置为非空值时跳过 MySQL 客户端依赖。
# 存在的意义是让「不需要 MySQL 驱动」的环境不必为它付出代价：libmysqlclient 在
# ConanCenter 上只有源码包，CI 或新机器上要现场编译十几分钟，而驱动本身有报错桩兜底
# （CMake 里 -DDATABASE_WITH_MYSQL=OFF），跳过它不影响其余功能与测试。
SKIP_MYSQL_DEPENDENCY_ENVIRONMENT_VARIABLE = "ASYN_SKIP_MYSQL_DEPS"


class ConanApplication(ConanFile):
    package_type = "application"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps"

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.user_presets_path = False
        tc.cache_variables["CMAKE_CXX_STANDARD"] = "20"
        tc.cache_variables["CMAKE_CXX_STANDARD_REQUIRED"] = "ON"
        tc.cache_variables["CMAKE_CXX_EXTENSIONS"] = "OFF"
        tc.generate()

    def requirements(self):
        skipMySQLDependency = bool(os.environ.get(SKIP_MYSQL_DEPENDENCY_ENVIRONMENT_VARIABLE))
        requirements = self.conan_data.get('requirements', [])
        for requirement in requirements:
            # jemalloc 在 Windows 上不可用，跳过
            if 'jemalloc' in requirement and self.settings.os == 'Windows':
                continue
            if 'libmysqlclient' in requirement and skipMySQLDependency:
                continue
            self.requires(requirement)
