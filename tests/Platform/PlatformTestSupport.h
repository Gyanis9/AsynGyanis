/**
 * @file PlatformTestSupport.h
 * @brief Platform 单元测试辅助：临时目录、文件写入与描述符可读性轮询
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/IO/FileDescriptor.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace AsynGyanis::Platform::TestSupport
{
    /**
     * @brief 临时目录夹具
     *
     * @details 在系统临时目录下创建带唯一后缀的目录，析构时递归删除，
     *          使每个用例互不干扰且失败退出也能清理干净。
     */
    class TemporaryDirectory
    {
    public:
        /**
         * @brief 创建临时目录
         * @param namePrefix 便于调试的用途前缀，如 "FileWatcher"
         */
        explicit TemporaryDirectory(const std::string &namePrefix)
        {
            static std::atomic<unsigned int> sequenceCounter{0};

            const auto salt = std::to_string(
                                      std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                              std::to_string(sequenceCounter.fetch_add(1));

            m_path = std::filesystem::temp_directory_path() /
                     ("AsynGyanis_Platform_" + namePrefix + "_" + salt);

            std::error_code error;
            std::filesystem::create_directories(m_path, error);
        }

        /**
         * @brief 析构并递归删除临时目录
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
         * @brief 在临时目录内写入一个文本文件
         * @param fileName 文件名（相对于临时目录）
         * @param content 文件内容
         * @return true 写入成功
         * @return false 打开或写入失败
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

    private:
        std::filesystem::path m_path; ///< 临时目录绝对路径
    };

    /**
     * @brief 轮询等待描述符变为可读
     *
     * @details 不使用 poll()/select()，因为两者在 Windows 上只面向 socket，
     *          这里以带超时的重复读取实现跨平台等待。
     * @param fileDescriptor 待观察的描述符
     * @param timeoutMilliseconds 最长等待毫秒数
     * @return true 在超时前成功读到数据
     * @return false 超时仍未读到数据
     */
    inline bool waitForReadable(const int fileDescriptor, const int timeoutMilliseconds)
    {
        constexpr int kpollIntervalMilliseconds = 5;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
        char       buffer[64];

        while (std::chrono::steady_clock::now() < deadline)
        {
            if (FileDescriptor::read(fileDescriptor, buffer, sizeof(buffer)) > 0)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kpollIntervalMilliseconds));
        }
        return false;
    }

    /**
     * @brief 轮询等待某个布尔条件成立
     * @tparam Predicate 可调用且返回 bool 的类型
     * @param predicate 条件判断对象
     * @param timeoutMilliseconds 最长等待毫秒数
     * @return true 条件在超时前成立
     * @return false 超时仍未成立
     */
    template<typename Predicate>
    bool waitForCondition(Predicate predicate, const int timeoutMilliseconds)
    {
        constexpr int kpollIntervalMilliseconds = 5;

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
} // namespace AsynGyanis::Platform::TestSupport
