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
         * @brief 取当前进程此刻占住的常驻内存字节数（RSS）
         * @details 这是「运行期内存采样」的最小读数：泄漏与缓存无界增长都先体现在它上面，而进程内没有
         *          别的通道能把它交出去（分配器统计要等 mimalloc 那一档才存在，且各家口径不同）。
         *          Windows 取工作集，POSIX 取 /proc/self/statm 的常驻页乘页尺寸。
         * @return std::uint64_t 常驻字节数；平台读不出时返回 0（按「不知道」处理，与 currentProcessId 同口径）
         * @note 分配器把空闲页面留在自己的池里时，释放内存不一定让这一列掉下来（glibc 就是如此），
         *       因此它读的是**趋势**而不是「当前活跃分配了多少字节」
         */
        static std::uint64_t residentMemoryBytes() noexcept;
    };
} // namespace AsynGyanis::Platform
