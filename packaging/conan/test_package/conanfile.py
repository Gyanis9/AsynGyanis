"""
@file conanfile.py
@brief asyngyanis 库包的消费方冒烟测试：conan create 会在打包完成后自动跑它
@author Gyanis
@date 2026-09-13
@version 1.0.0
@copyright Copyright (c) . All rights reserved.

它做的事就是**像外部工程那样**用这个包：requires 包本身、拿 CMakeDeps 生成的配置
find_package(AsynGyanis)、链 AsynGyanis::Net，然后跑起来。因此「包能装」与「包能用」
是同一件事的两面——后者才是真问题（少了某个组件的依赖、头文件路径不对，
都会在这里当场暴露）。
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
