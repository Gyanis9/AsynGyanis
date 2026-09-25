# asyn-gyanis 的 vcpkg 端口：从仓库的发布标签取源码，用本项目自带的 CMake 包配置导出五个模块。
#
# 为什么走 git 而不是归档 tarball：仓库没有对外公开的源码归档入口（tarball 里的子模块与
# 预生成夹具不稳定），而标签是本项目发布流程里唯一被钉住的真值（v<主>.<次>.<修订>，与
# CHANGELOG 和 CMake 版本号同解，由 scripts/check-release-version.py 把关）。
#
# 两件本端口刻意不做，用到之前别当它存在：
# - **MySQL 驱动**：vcpkg 没有 libmysqlclient 这个端口（只有需要手工接受 Oracle 许可的
#   libmysql），而本项目的 MySQL 客户端本来就按可选依赖处理（探测不到即编成「每个入口给中文
#   错误」的桩），所以这里显式传 -DDATABASE_WITH_MYSQL=OFF，与 Conan 侧的
#   ASYN_SKIP_MYSQL_DEPS 同一条口径。
# - **版权文件**：vcpkg_install_copyright 需要一个 LICENSE 文件，而仓库目前没有（README 的
#   「版权」一节写了 MIT，但没有落成文件）。要把它推到公共注册表之前，得先由作者补上那份文件。

vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL "git@github.com:Gyanis9/AsynGyanis.git"
    # 标签 v1.1.0 所指的那个提交。vcpkg 的 git 源码是「浅取一个提交」的形状：REF 要写死成
    # 提交号，FETCH_REF 才是用来把那条标签拉下来的引用。版本号与本仓库的 project(VERSION)
    # 和标签同解，改版本时三处一起改（scripts/check-release-version.py 会盯后两处）。
    REF "9dc1ba6c0633dc046068f74b8eda107563e2bd69"
    FETCH_REF "refs/tags/v1.1.0"
    HEAD_REF develop
)

vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        io-uring ASYN_WITH_IO_URING
        mimalloc ASYN_WITH_MIMALLOC
)

# 测试与示例不进包：它们要么依赖第三方裁判库（nghttp3）、要么需要真机数据库与凭据，
# 都不是消费方要的东西。安装规则必须开着——本端口全靠它导出目标。
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DASYN_BUILD_TESTS=OFF
        -DASYN_BUILD_SAMPLES=OFF
        -DASYN_ENABLE_INSTALL=ON
        -DASYN_ENABLE_COVERAGE=OFF
        -DENABLE_SANITIZERS=OFF
        -DSANITIZE_THREADS=OFF
        -DDATABASE_WITH_MYSQL=OFF
        ${FEATURE_OPTIONS}
)

vcpkg_cmake_install()

# vcpkg 的惯例是把消费提示放在 share/<端口名>/usage，装完包会随 `installed --debug` 与
# 消费方的提示一起给出；本项目自己的安装规则不带这一份（它带的是 cmake/Findbrotli.cmake 那种
# 兜底模块），所以由端口补装
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

vcpkg_cmake_config_fixup(
    PACKAGE_NAME AsynGyanis
    CONFIG_PATH lib/cmake/AsynGyanis
)
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

# 只留一份静态库时 vcpkg 仍会检查 bin/ 里没有多余产物；本项目不装可执行文件，
# 这里顺手把空目录摘掉，免得消费方以为有命令行工具可用
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/bin" "${CURRENT_PACKAGES_DIR}/debug/bin")
