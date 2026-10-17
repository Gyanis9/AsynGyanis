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

# 配置样例把日志与链路文件的目录写成绝对路径 /var/lib/asyngyanis/logs，而这个账号是
# --no-create-home 建的、家目录都不存在：目录得在这里备好并归属它，否则滚动文件 sink 建不出
# 目录，随后开文件时抛带路径的中文异常、服务起不来。
# 单独一层、不并进上面的 apt 那条：改这一格不必重新联网装包。
RUN install -d -o asyngyanis -g asyngyanis /var/lib/asyngyanis

# 静态 Linux 构建把示例落在 samples/ 下（bin/ 只在共享 Windows 形态才启用）
COPY --from=builder /src/build/release/samples/ReferenceServer /usr/local/bin/ReferenceServer

USER asyngyanis
EXPOSE 8080

# 健康检查只用 runtime 阶段已有的 bash /dev/tcp 探监听端口：不额外装 curl（少一层攻击面与体积），
# 也不需要运维令牌。默认 CMD 不带 --config，运维端点（/healthz）没注册，因此这里刻意只判
# 「端口还在接受连接」；要判 HTTP 应答就自己带 --metrics 或配置 expose_metrics 后改 PROBE_PATH。
# 30s 间隔：本机回环探测成本可忽略，又能在 90s（3 次失败）内定性「进程还在但不再应答」；
# 3s 超时防一次探测挂住；5s 起始宽限覆盖静态二进制加载与监听器 bind。
ENV ASYN_HEALTHCHECK_PORT=8080
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD ["/bin/bash", "-c", "exec 3<>/dev/tcp/127.0.0.1/${ASYN_HEALTHCHECK_PORT}"]

# 密文/证书、限额与日志配置走 --config（另挂进容器）；这里给的是不依赖外部文件的明文形态。
# 生产配置样例见 packaging/asyngyanis.yaml.example（挂载为 --config 指的文件）。
# 监听地址只能从命令行给（配置里没有 host/port 两格），而 --host 的默认值是 localhost：
# `docker run 镜像 --config /cfg/x.yaml` 会**整段替换**下面这行 CMD，于是服务只听回环、
# 从容器外连不上。带 --config 时要连 --host 0.0.0.0 一起给。
ENTRYPOINT ["/usr/local/bin/ReferenceServer"]
CMD ["--host", "0.0.0.0", "--port", "8080"]
