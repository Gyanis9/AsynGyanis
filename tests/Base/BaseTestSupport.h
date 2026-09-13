/**
 * @file BaseTestSupport.h
 * @brief Base 模块单元测试辅助：临时目录夹具与配置目录构造
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace AsynGyanis::Base::TestSupport
{
    /**
     * @brief 临时目录夹具
     *
     * @details 在系统临时目录下创建唯一命名的目录，析构时递归删除。
     *          日志文件 Sink、配置目录扫描与热加载用例都依赖一个可写的空目录。
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
                m_path = std::filesystem::temp_directory_path() / ("AsynGyanis_Base_" + namePrefix + "_" + salt);
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
         * @brief 析构并递归删除临时目录，忽略删除失败
         */
        ~TemporaryDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
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
     * @brief 轮询等待某个布尔条件成立
     * @tparam Predicate 可调用且返回 bool 的类型
     * @param predicate 条件判断对象
     * @param timeoutMilliseconds 最长等待毫秒数
     * @return true 条件在超时前成立
     */
    template<typename Predicate>
    bool waitForCondition(Predicate predicate, const int timeoutMilliseconds)
    {
        constexpr int kpollIntervalMilliseconds = 10;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kpollIntervalMilliseconds));
        }
        return false;
    }
} // namespace AsynGyanis::Base::TestSupport
