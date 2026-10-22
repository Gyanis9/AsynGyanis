import os

from conan import ConanFile
from conan.tools.cmake import cmake_layout, CMakeToolchain

# 环境变量开关：置为非空值时跳过 MySQL 客户端依赖。libmysqlclient 在 ConanCenter 上只有
# 源码包，CI 或新机器要现场编译十几分钟；驱动本身有报错桩兜底（-DDATABASE_WITH_MYSQL=OFF），
# 跳过它不影响其余功能与测试。
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
        # C++ 标准只在根 CMakeLists.txt 定义：那里用普通变量写 CMAKE_CXX_STANDARD 23，
        # 会盖掉这里的缓存值，重复传只会得到两个答案。
        tc.generate()

    def requirements(self):
        skipMySQLDependency = bool(os.environ.get(SKIP_MYSQL_DEPENDENCY_ENVIRONMENT_VARIABLE))
        requirements = self.conan_data.get('requirements', [])
        for requirement in requirements:
            if 'libmysqlclient' in requirement and skipMySQLDependency:
                continue
            self.requires(requirement)
