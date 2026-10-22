# 示例程序：每个子系统一个独立可运行的程序，各自自检并以退出码报告结果。
# 各示例按模块拆开，scripts/run_samples.py 全量运行并按退出码判定系统完整性。

# 一个示例一个目标：目标名与程序名同为 PascalCase（CoreLoop、ReferenceServer…），是对外名字，
# 文档、脚本、CI 与按可执行体名认进程的判据都按它引用；唯一偏离是 PlatformPrimitives：
# Platform 已是库目标，CMake 两目标不能同名，故按文件名取，而不是模块名。
function(asyn_add_sample sample_name)
        add_executable(${sample_name} ${ARGN})
        target_link_libraries(${sample_name} PRIVATE Base Core Net)
        target_include_directories(${sample_name} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(${sample_name} PRIVATE cxx_std_23)
        # ASan 运行库拷到本目标输出目录（见 cmake/AsynGyanisSanitizers.cmake）
        asyn_copy_sanitizer_runtime(${sample_name})
endfunction()

# 部署形态参考服务端：h1/h2/h2c/h3/WS/SSE 全开，带中间件、追踪、静态文件、指标、限流、
# 多进程与优雅停机。
asyn_add_sample(ReferenceServer Net/ReferenceServer.cpp)
# /json 的 version 取自根 CMakeLists 的 project(... VERSION ...)，不写死。
target_compile_definitions(ReferenceServer PRIVATE ASYN_PROJECT_VERSION="${PROJECT_VERSION}")

# Base：日志子系统（Sink、格式化器、注册表、配置驱动装配、异常带栈）
asyn_add_sample(BaseLog Base/Log.cpp)

# Base：配置子系统（多文件加载、点分路径取值、模式校验、热重载）
asyn_add_sample(BaseConfig Base/Config.cpp)

# Platform：套接字工具、描述符、通知器、定时器、内存映射、数据报、进程起停与平台杂项
asyn_add_sample(PlatformPrimitives Platform/Primitives.cpp)
target_link_libraries(PlatformPrimitives PRIVATE Platform)

# Core：事件循环与线程池、调度器、定时器、IO 监听器、帧池、取消令牌、解析与 UDP/TCP 协程收发
asyn_add_sample(CoreLoop Core/Loop.cpp)

# Core TLS：加固上下文、证书装载与热替换、OCSP、私有 CA 与双向 TLS 的回环握手
asyn_add_sample(CoreTls Core/Tls.cpp)

# Core 多进程：WorkerSupervisor 的起停、崩溃补位与「起来就崩」的次数上限
asyn_add_sample(CoreWorker Core/Worker.cpp)

# Net HTTP/1.1：路由与中间件、解析上限、分块与 SSE、WebSocket、限额、观测端点、优雅收口
asyn_add_sample(NetHttpDemo Net/Http.cpp)

# Net TLS 与 HTTP/2：ALPN 协商、h2c 明文、多路复用、GOAWAY 排空与解析上限
asyn_add_sample(NetHttpsH2Demo Net/HttpsH2.cpp)

# Net HTTP/3：QUIC 服务端的配置校验、UDP 端口起停、畸形报文容错、定时驱动与排空
asyn_add_sample(NetHttp3Demo Net/Http3.cpp)

# Net UDP：UdpServer 按地址 bind 或接手已绑端口、逐条交付、主动下发与收口
asyn_add_sample(NetUdpDemo Net/Udp.cpp)
target_link_libraries(NetUdpDemo PRIVATE Platform)

# Net 网关：入站 HttpServer 与出站 HttpClient 拼成反向代理；健康检查与加权轮转见末尾 gated
asyn_add_sample(NetGateway Net/Gateway.cpp)

# Database：SQLite 文件/内存库、ORM、事务、二进制列、连接池；MySQL/Redis 按环境变量门控
asyn_add_sample(DatabaseDemo Database/OrmAndPool.cpp)
target_link_libraries(DatabaseDemo PRIVATE Database)

# Core 零停机换代：把在监听套接字交给新一代进程，端口全程不关（仅 POSIX）。
# Windows 卡在完成端口：套接字与端口的关联是内核对象级且不可解除，新一代挂载被拒（实测 87）。
# POSIX 前提：通道必须 AF_UNIX（TCP 通道内核静默丢描述符），交棒方收口不对端点 shutdown。
if (NOT WIN32)
        asyn_add_sample(CoreUpgrade Core/Upgrade.cpp)
        target_link_libraries(CoreUpgrade PRIVATE Platform)
endif()
