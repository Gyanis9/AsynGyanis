# 示例程序：每个子系统一个可独立运行的程序，各自自检并以退出码报告结果
#
# reference_server 是「部署形态」的参考服务端（h1/h2/h2c/h3/WS/SSE 全开，带中间件、追踪、静态文件、
# 指标、限流、多进程与优雅停机），保留用于压测、上手，以及当作业务服务的起步形状；
# 其余按模块拆开的示例承担「把每一块单独跑通」的职责，脚本 scripts/run_samples.py 会把它们
# 全部跑一遍并按退出码判定系统完整性。

# 一个示例一个目标。目录按**模块**分（Base / Platform / Core / Net / Database，与 src/ 与 tests/ 同一套名字），
# 文件名就是这个示例讲的那件事（ReferenceServer.cpp、Loop.cpp、Upgrade.cpp…）——不再用 main.cpp：
# 一排同名文件在编辑器标签页与调用栈里分不出谁是谁，而「哪个模块的哪个面」本该从路径上一眼读得出来。
# 目标名一般不动（core_loop、net_http_demo…）：那是对外的程序名，文档、脚本、压测与 CI 都按它引用，
# 改它属于换接口而不是改布局。唯一的例外是 reference_server（原名 echo_server）：那名字不准——它一个
# echo 端点都没有，挂的是 /json /trace /bench /big /sse /ws，所以连程序名一起换，全仓引用同日跟上。
# common/ 统一挂作头文件搜索路径。
function(asyn_add_sample sample_name)
        add_executable(${sample_name} ${ARGN})
        target_link_libraries(${sample_name} PRIVATE Base Core Net)
        target_include_directories(${sample_name} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(${sample_name} PRIVATE cxx_std_23)
        # ASan 运行库拷到本目标输出目录（见根 CMakeLists 的 asyn_copy_sanitizer_runtime）
        asyn_copy_sanitizer_runtime(${sample_name})
endfunction()

# 部署形态的参考服务端（原名 echo_server：它没有 echo 端点，见上面第 10 行的例外说明）
asyn_add_sample(reference_server Net/ReferenceServer.cpp)

# Base：日志子系统（Sink、格式化器、注册表、配置驱动装配、异常带栈）
asyn_add_sample(base_log Base/Log.cpp)

# Base：配置子系统（多文件加载、点分路径取值、模式校验、热重载）
asyn_add_sample(base_config Base/Config.cpp)

# Platform：套接字工具、描述符、通知器、定时器、内存映射、数据报、进程起停与平台杂项
asyn_add_sample(platform Platform/Primitives.cpp)
target_link_libraries(platform PRIVATE Platform)

# Core：事件循环与线程池、调度器、定时器、IO 监听器、帧池、取消令牌、解析与 UDP/TCP 协程收发
asyn_add_sample(core_loop Core/Loop.cpp)

# Core TLS：加固上下文、证书装载与热替换、OCSP、私有 CA 与双向 TLS 的回环握手
asyn_add_sample(core_tls Core/Tls.cpp)

# Core 多进程：WorkerSupervisor 的起停、崩溃补位与「起来就崩」的次数上限
asyn_add_sample(core_worker Core/Worker.cpp)

# Net HTTP/1.1：路由与中间件、解析上限、分块与 SSE、WebSocket、限额、观测端点、优雅收口
asyn_add_sample(net_http_demo Net/Http.cpp)

# Net TLS 与 HTTP/2：ALPN 协商、h2c 明文、多路复用、GOAWAY 排空与解析上限
asyn_add_sample(net_https_h2_demo Net/HttpsH2.cpp)

# Net HTTP/3：QUIC 服务端的配置校验、UDP 端口起停、畸形报文容错、定时驱动与排空
asyn_add_sample(net_http3_demo Net/Http3.cpp)

# Net UDP：UdpServer 的两种起步方式（按地址 bind / 接手别人绑好的口）、逐条交付、主动下发、统计与收口
asyn_add_sample(net_udp_demo Net/Udp.cpp)
target_link_libraries(net_udp_demo PRIVATE Platform)

# Database：SQLite 文件库/内存库、ORM 与查询构造、事务、二进制列、参数绑定、连接池；MySQL/Redis 按环境变量门控
asyn_add_sample(database_demo Database/OrmAndPool.cpp)
target_link_libraries(database_demo PRIVATE Database)

# Core 零停机换代：把正在监听的套接字交给新一代进程，端口全程不关（POSIX 才构建，理由见下）
#
# 移交本身两家都有原语（POSIX 走 SCM_RIGHTS，Windows 走 WSADuplicateSocket），拦住 Windows 的是
# 事件后端：套接字与完成端口的关联是**内核对象级**且不可解除的，交棒方的循环一开跑就把监听句柄挂上了
# 自己的端口，新一代再把它挂进自己的完成端口就被拒（实测 CreateIoCompletionPort 报 87，父代答完话、
# 子代一次都没接上）。那条拒因现在带得出码，用例是 IoWatcher.DescriptorOnAnotherCompletionPortThrowsWithWin32Reason。
# 要让 Windows 也走得通，得给换代单独配一条「阻塞 accept 线程 + adoptConnection」的接手路径，
# 那是另一件工程，不该由这条示例顺带宣布支持。
#
# POSIX 侧成立的前提有两条，两条都是实测换来的：通道必须 AF_UNIX（TCP 通道上内核静默丢描述符），
# 以及交棒方收口时不对端点做 shutdown（见 AsyncSocket::markAsListening()）。
if (NOT WIN32)
        asyn_add_sample(core_upgrade Core/Upgrade.cpp)
        target_link_libraries(core_upgrade PRIVATE Platform)
endif()
