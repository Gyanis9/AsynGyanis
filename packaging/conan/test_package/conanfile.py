"""asyngyanis 库包的消费方冒烟测试，conan create 打包完成后自动跑它。

像外部工程那样 requires 包本身，用 CMakeDeps 的配置 find_package(AsynGyanis) 并链 AsynGyanis::Net，
然后运行；组件依赖缺失或头文件路径不对会在这里当场暴露。
"""

import os

from conan import ConanFile
from conan.tools.build import can_run
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class AsynGyanisTestPackage(ConanFile):
    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        # 被测包由 conan create 注入：这里链的是刚打出来的那一个，不是缓存里的旧版本
        self.requires(self.tested_reference_str)

    def layout(self):
        cmake_layout(self)

    def generate(self):
        CMakeDeps(self).generate()
        CMakeToolchain(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def test(self):
        # 交叉编译时不跑，只证明能链上；本机验证必须真的执行一次
        if can_run(self):
            executable_name = "consumer_smoke.exe" if self.settings.os == "Windows" else "consumer_smoke"
            self.run(os.path.join(self.cpp.build.bindirs[0], executable_name), env="conanrun")
