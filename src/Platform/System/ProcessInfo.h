/**
 * @file ProcessInfo.h
 * @brief 进程级平台信息：可执行文件目录与环境变量读取
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace AsynGyanis::Platform
{
    /**
     * @brief 当前进程的平台信息入口
     *
     * @details 日志与配置的相对路径必须基于可执行文件所在目录解析，否则会随
     *          进程启动时的工作目录变化而漂移；环境变量读取在 MSVC 上必须使用
     *          _dupenv_s 才能避开 C4996 与内存归属问题，两者统一由本类承担。
     *
     * @note 磁盘与网络级指标不做（这是决定，不是待办）：本类只提供进程级读数（RSS、CPU 秒、
     *       句柄数、启动时刻）——它们有确定的消费方（`/metrics` 与 OTLP 两条出口，见
     *       `Core::ProcessMetricsRegistry` 与 `HttpServerStats`）。磁盘/网络那两组没有这样的消费方：
     *       验收侧的磁盘与链路对账直接用 `/proc`（`/proc/self/io`、`/proc/net/dev`、`/proc/diskstats`）
     *       现取现算，采集侧要的是 `node_exporter` 口径的整机读数而不是某个进程的份额，而本层给出的
     *       只会是「本进程读写了多少字节」这种半张脸的口径。加一组没人读的 API 等于给下一个读代码的
     *       人留一个「这里为什么没有消费方」的疑问；真出现消费方（例如要做进程级 IO 限额或配额告警）
     *       时按同一份形状加：`ioReadBytes()` / `ioWriteBytes()`（POSIX 走 /proc/self/io，Windows 走
     *       GetProcessIoCounters），网络收发那两格 Windows 侧只有 `GetPerTcpConnectionEStats` 这类
     *       按连接的接口，进程级口径本就取不到——先确认需求再谈落点。
     */
    class ASYN_PLATFORM_API ProcessInfo
    {
    public:
        /**
         * @brief 获取可执行文件所在目录
         * @return std::filesystem::path 绝对目录路径；平台查询失败时返回空路径
         */
        static std::filesystem::path applicationDirectory();

        /**
         * @brief 读取当前进程的环境变量
         * @param variableName 环境变量名
         * @return std::optional<std::string> 变量存在时返回其值，未定义时返回 std::nullopt
         */
        static std::optional<std::string> environmentVariable(const std::string &variableName);

        /**
         * @brief 枚举名字带指定前缀的环境变量
         * @details 部署侧要按前缀批量覆盖配置，只能整表扫——让调用方逐个键去猜变量名会把命名规则
         *          钉死在平台层之外，且每次加键都要改代码。Windows 走 GetEnvironmentStringsW
         *          （整块以两个连续 NUL 结尾，名字与值之间只在第一个 '=' 处切一次，值里的 '=' 属于值本身；
         *          首字符即 '=' 的是「某驱动器当前目录」这类内部变量，没有合法名字，直接跳过），
         *          取回的宽字符统一转成 UTF-8；POSIX 遍历 extern environ。
         * @param prefix 名字前缀（含分隔符本身，如 "ASYN_"）；空串按「不过滤」处理，返回全部变量
         * @return std::vector<std::pair<std::string, std::string>> 名字与值的配对；名字保持平台给出的
         *         原始大小写（转小写这类语义是调用方的规则，不属于本层）
         */
        static std::vector<std::pair<std::string, std::string>> environmentVariablesWithPrefix(const std::string &prefix);

        /**
         * @brief 取当前进程的进程号
         * @details 日志与诊断要能区分「哪个进程写的」：多进程 worker 模型下同一份配置会跑出多个进程，
         *          没有进程号就只能靠时间顺序猜。
         * @return long 进程号；平台调用失败时返回 0（调用方按「不知道」处理，不要当成合法进程号）
         */
        static long currentProcessId() noexcept;

        /**
         * @brief 取当前线程的指纹（诊断与崩溃现场用）
         * @details 崩溃现场要回答「是哪条线程死的」：进程号只能定位到进程，多线程下一次崩溃
         *          不带上线程身份就无法与日志里的线程活动对上。
         * @return std::uint64_t POSIX 上是 pthread_t 的数值，Windows 上是线程 ID；不分配
         */
        static std::uint64_t currentThreadFingerprint() noexcept;

        /**
         * @brief 取当前进程此刻占住的常驻内存字节数（RSS）
         * @details 这是「运行期内存采样」的最小读数：泄漏与缓存无界增长都先体现在它上面，而进程内没有
         *          别的通道能把它交出去（分配器统计要等 mimalloc 那一档才存在，且各家口径不同）。
         *          Windows 取工作集，POSIX 取 /proc/self/statm 的常驻页乘页尺寸。
         * @return std::uint64_t 常驻字节数；平台读不出时返回 0（按「不知道」处理，与 currentProcessId 同口径）
         * @note 分配器把空闲页面留在自己的池里时，释放内存不一定让这一列掉下来（glibc 就是如此），
         *       因此它读的是趋势而不是「当前活跃分配了多少字节」
         */
        static std::uint64_t residentMemoryBytes() noexcept;

        /**
         * @brief 取当前进程累计消耗的 CPU 时间（用户态加内核态）
         * @details 与常驻内存同属「泄漏与饱和」的最小读数：Prometheus 惯例里 process_cpu_seconds_total
         *          是计数器，配合 rate() 看占用趋势。Windows 取 GetProcessTimes 的 user+kernel，
         *          POSIX 取 getrusage(RUSAGE_SELF) 两个字段的和。
         * @return std::uint64_t 累计 CPU 时间的整数秒；平台读不出时返回 0（按「不知道」处理）
         * @note 只交出整数秒：计数器口径下 rate() 会把小数抹平，短命进程早段读到 0 属语义而非缺陷
         */
        static std::uint64_t cpuSeconds() noexcept;

        /**
         * @brief 取当前进程此刻打开的句柄/文件描述符数量
         * @details 连接、文件、监听口泄漏的通用信号。Windows 取 GetProcessHandleCount，
         *          POSIX 数 /proc/self/fd 的目录项（含列举本身占的那一个，噪声 ±1）。
         * @return std::uint64_t 当前打开的句柄数；平台读不出时返回 0（按「不知道」处理）
         */
        static std::uint64_t openHandleCount() noexcept;

        /**
         * @brief 取当前进程的启动时刻（Unix epoch 秒）
         * @details Prometheus 惯例里 process_start_time_seconds 用来识别「进程重启过」——计数器回落时
         *          有它才能判断是新进程从零开始还是真的掉了。Windows 取 GetProcessTimes 的创建时间；
         *          POSIX 由 /proc/self/stat 的 starttime 节拍数加 /proc/stat 的 btime 折算。
         * @return std::uint64_t 启动时刻（epoch 秒）；平台读不出时返回 0（按「不知道」处理）
         */
        static std::uint64_t startTimeSeconds() noexcept;
    };
} // namespace AsynGyanis::Platform
