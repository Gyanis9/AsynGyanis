# AsynGyanis 的多阶段部署镜像。
#
# 构建（在仓库根执行；构建上下文必须包含源码与 conandata.yml）：
#   docker build -f packaging/Dockerfile -t asyngyanis:local .
# 运行：
#   docker run --rm -p 8080:8080 asyngyanis:local
#
# 依赖在 builder 阶段按 conandata.yml 现装（走 conan_provider.cmake），不把本机 .conan2
# 缓存 COPY 进镜像：那会把开发机的 ABI 与路径一起带进交付件。

FROM ubuntu:24.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        ninja-build \
        perl \
        pkg-config \
        python3 \
        python3-pip \
        python3-venv \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m venv /opt/conan-venv \
    && /opt/conan-venv/bin/pip install --no-cache-dir "conan>=2.25" \
    && /opt/conan-venv/bin/conan profile detect --force
ENV PATH="/opt/conan-venv/bin:${PATH}"

WORKDIR /src
COPY . .

# 部署镜像不需要测试与可选的 MySQL 驱动：跳过它们能显著缩短构建，也避免把
# libmysqlclient 的源码编译塞进一个只跑 HTTP 服务的镜像。conandata.yml 仍是依赖的唯一事实。
ENV ASYN_SKIP_MYSQL_DEPS=1
RUN cmake --preset release \
        -DASYN_BUILD_TESTS=OFF \
        -DDATABASE_WITH_MYSQL=OFF \
    && cmake --build build/release --target ReferenceServer -- -k 0

FROM ubuntu:24.04 AS runtime

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --no-create-home --shell /usr/sbin/nologin asyngyanis

# 静态 Linux 构建把示例落在 samples/ 下（bin/ 只在共享 Windows 形态才启用）
COPY --from=builder /src/build/release/samples/ReferenceServer /usr/local/bin/ReferenceServer

USER asyngyanis
EXPOSE 8080

# 密文/证书、限额与日志配置走 --config（另挂进容器）；这里给的是不依赖外部文件的明文形态。
ENTRYPOINT ["/usr/local/bin/ReferenceServer"]
CMD ["--host", "0.0.0.0", "--port", "8080"]
