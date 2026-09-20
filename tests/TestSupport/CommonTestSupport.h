/**
 * @file CommonTestSupport.h
 * @brief 跨模块共用的测试夹具：临时目录、轮询等待与栈帧符号判定
 * @author Gyanis
 * @date 2026-09-18
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 本头不依赖任何模块的生产代码，供 Base / Core / Platform 等模块的测试目标包含。
 *          各模块的 TestSupport 命名空间以 using 转发暴露这些名字，既有调用点无需改动；
 *          此前 Base 与 Platform 各存一份近乎相同的实现，现只有这一处定义。
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace AsynGyanis::TestSupport
{
    /// 等待类断言的统一上限：正常耗时都在毫秒级，给足余量但不许无界等待
    inline constexpr std::chrono::milliseconds kWaitTimeout{5000};

    /**
     * @brief 临时目录夹具
     *
     * @details 在系统临时目录下创建唯一命名的目录，析构时递归删除。
     *          文件 Sink、配置目录扫描与文件监视等用例都依赖一个可写的空目录。
     */
    class TemporaryDirectory
    {
    public:
        /**
         * @brief 创建临时目录
         * @param namePrefix 便于定位问题的用途前缀，如 "FileSink"
         */
        explicit TemporaryDirectory(const std::string &namePrefix)
        {
            static std::atomic<unsigned int> sequenceCounter{0};

            // 目录名必须**跨进程**唯一：ctest 会把每个用例作为独立进程并行拉起，而 steady_clock 的读数
            // 是全系统共享的、序号计数器又是每个进程各自从 0 开始，两者相加仍可能撞名（撞名时两个进程
            // 会共用同一个目录，先结束的那个 remove_all 会把另一个的用例文件删掉）。因此以
            // create_directories 是否**真的新建了目录**为准，撞了就换个盐重试——不依赖时钟精度
            std::error_code error;
            while (true)
            {
                const std::string salt = std::to_string(
                                                 std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                                         std::to_string(sequenceCounter.fetch_add(1));
                m_path = std::filesystem::temp_directory_path() / ("AsynGyanis_Test_" + namePrefix + "_" + salt);
                if (std::filesystem::create_directories(m_path, error))
                {
                    break;
                }
                if (error)
                {
                    // 真出错（权限等）不再重试：保持原语义，让用例自己去失败并暴露环境问题
                    break;
                }
            }
        }

        /**
         * @brief 析构并递归删除临时目录；删除失败时向 stderr 报告原因
         */
        ~TemporaryDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
            // 删除失败必须说出来：静默失败会让「句柄没释放」这类问题以临时目录在 %TEMP% 里
            // 悄悄堆积的形式潜伏（曾有 2000+ 个目录累积数天无人察觉）
            if (error)
            {
                std::fprintf(stderr, "[测试支持] 临时目录删除失败（可能仍被打开的文件占用）：%s — %s\n",
                             m_path.string().c_str(), error.message().c_str());
            }
        }

        TemporaryDirectory(const TemporaryDirectory &) = delete;

        TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

        /**
         * @brief 获取临时目录路径
         * @return const std::filesystem::path& 目录绝对路径
         */
        [[nodiscard]] const std::filesystem::path &path() const noexcept
        {
            return m_path;
        }

        /**
         * @brief 在临时目录内写入文本文件
         * @param fileName 相对文件名
         * @param content 文件内容
         * @return true 写入成功
         */
        bool writeFile(const std::string &fileName, const std::string &content) const
        {
            std::ofstream file(m_path / fileName, std::ios::out | std::ios::trunc);
            if (!file.is_open())
            {
                return false;
            }
            file << content;
            return file.good();
        }

        /**
         * @brief 在临时目录的子路径下写入文本文件
         * @param relativePath 相对于临时目录的路径，缺失的父目录会被创建
         * @param content 文件内容
         * @return true 写入成功
         */
        bool writeNestedFile(const std::string &relativePath, const std::string &content) const
        {
            const std::filesystem::path targetPath = m_path / relativePath;

            std::error_code error;
            std::filesystem::create_directories(targetPath.parent_path(), error);

            std::ofstream file(targetPath, std::ios::out | std::ios::trunc);
            if (!file.is_open())
            {
                return false;
            }
            file << content;
            return file.good();
        }

    private:
        std::filesystem::path m_path; ///< 临时目录绝对路径
    };

    /**
     * @brief 在时限内轮询等待条件成立（避免固定 sleep 造成的偶发失败）
     * @tparam Predicate 可调用且返回 bool 的类型
     * @param predicate 待轮询的条件
     * @param timeout 超时上限，默认 kWaitTimeout
     * @return true 条件在时限内成立
     */
    template<typename Predicate>
    bool waitForCondition(Predicate predicate, const std::chrono::milliseconds timeout = kWaitTimeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    /**
     * @brief 以毫秒整数给出超时上限的重载
     * @details int 不会隐式转成 std::chrono::milliseconds（duration 的 rep 构造是 explicit 的），
     *          但各模块的既有调用点大量直接传毫秒整数，这里提供同名重载避免调用方被迫包一层。
     * @tparam Predicate 可调用且返回 bool 的类型
     * @param predicate 待轮询的条件
     * @param timeoutMilliseconds 最长等待毫秒数
     * @return true 条件在时限内成立
     */
    template<typename Predicate>
    bool waitForCondition(Predicate predicate, const int timeoutMilliseconds)
    {
        return waitForCondition(std::move(predicate), std::chrono::milliseconds(timeoutMilliseconds));
    }

    /**
     * @brief 判断调用栈文本里的帧是否已被符号解析出源文件位置
     * @details 没有调试信息时 MSVC 只给「模块名+0x偏移」、GCC 给「??」，文本照样非空——
     *          因此「文本为空」不能当作「符号不可用」的判据，只能看有没有出现源文件名。
     * @param stackTraceText 栈帧文本（Base::formatStackTrace() 的产出）
     * @return true 至少有一帧带出了源文件名
     */
    [[nodiscard]] inline bool hasResolvedStackTraceFrames(const std::string_view stackTraceText) noexcept
    {
        // 两家工具链的行号写法不同：MSVC 是 file.cpp(12)，GCC 是 file.cpp:12；头文件里的内联帧同理
        for (const std::string_view fileExtension: {".cpp", ".cc", ".hpp", ".h(", ".h:"})
        {
            if (stackTraceText.find(fileExtension) != std::string_view::npos)
            {
                return true;
            }
        }
        return false;
    }
} // namespace AsynGyanis::TestSupport
