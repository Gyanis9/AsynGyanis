/**
 * @file FileWatcher.h
 * @brief 文件变更监听器抽象接口与平台实现工厂
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/Platform.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace AsynGyanis::Platform
{
    /**
     * @brief 文件变更事件类型
     */
    enum class FileChangeType : std::uint8_t
    {
        Modified, ///< 文件内容被修改
        Created,  ///< 文件被创建或原子替换后落位
        Deleted,  ///< 文件被删除
        Moved     ///< 文件被移动或重命名（源路径）
    };

    /**
     * @brief 文件变更回调函数类型
     * @param filePath 发生变更的文件绝对路径
     * @param changeType 变更事件类型
     * @note 回调在监听线程上执行，实现方不得在回调内阻塞过久或直接销毁监听器
     */
    using FileChangeCallback = std::function<void(std::string_view filePath, FileChangeType changeType)>;

    /**
     * @brief 文件变更监听器抽象接口
     *
     * @details 屏蔽 Linux inotify 与 Windows ReadDirectoryChangesW 的差异，
     *          上层（如配置热加载）只依赖本接口。
     * @note 通过 create() 获取当前平台实例；实例不可拷贝与移动，
     *       析构前需保证已调用 stop()（各实现的析构函数会自行停止）。
     */
    class FileWatcher
    {
    public:
        /**
         * @brief 创建当前平台对应的监听器实现
         * @return std::unique_ptr<FileWatcher> Linux 返回 InotifyFileWatcher，
         *         Windows 返回 Win32FileWatcher
         * @throws std::runtime_error 平台原生监听句柄初始化失败
         */
        static std::unique_ptr<FileWatcher> create();

        virtual ~FileWatcher() = default;

        FileWatcher(const FileWatcher &)            = delete;
        FileWatcher &operator=(const FileWatcher &) = delete;
        FileWatcher(FileWatcher &&)                 = delete;
        FileWatcher &operator=(FileWatcher &&)      = delete;

        /**
         * @brief 启动监听线程
         * @return true 监听线程已运行（重复调用同样返回 true）
         * @return false 底层句柄无效或线程创建失败
         */
        virtual bool start() = 0;

        /**
         * @brief 停止监听线程并等待其完全退出
         */
        virtual void stop() = 0;

        /**
         * @brief 添加要监听的文件或目录
         * @param path 文件或目录路径，内部统一转为绝对路径
         * @param recursive 是否递归监听子目录，仅对目录有效
         * @return true 添加成功或已在监听集合中
         * @return false 路径无法解析或原生监听注册失败
         */
        virtual bool addWatch(std::string_view path, bool recursive = false) = 0;

        /**
         * @brief 移除指定路径的监听
         * @param path 之前添加过的文件或目录路径
         * @return true 移除成功
         * @return false 该路径未在监听集合中
         */
        virtual bool removeWatch(std::string_view path) = 0;

        /**
         * @brief 设置文件变更回调
         * @param callback 回调函数对象，可在监听运行期间安全替换
         */
        virtual void setCallback(FileChangeCallback callback) = 0;

        /**
         * @brief 查询监听线程是否正在运行
         * @return true 正在运行
         * @return false 未启动或已停止
         */
        [[nodiscard]] virtual bool isRunning() const noexcept = 0;

        /**
         * @brief 设置同一文件连续事件的防抖间隔
         * @details 由基类统一实现，两侧平台实现共用同一份防抖状态。
         * @param interval 防抖间隔，间隔内的重复事件被丢弃；非正值表示不防抖
         */
        void setDebounceInterval(std::chrono::milliseconds interval) noexcept;

    protected:
        /**
         * @brief 构造函数仅供平台实现类调用
         */
        FileWatcher() = default;

        /**
         * @brief 判断某路径的本次变更是否应当派发
         * @details 实现「同一路径在防抖窗口内只触发一次」的公共语义，并在防抖表
         *          超过规模上限时清理已过期记录，避免递归监听大目录树时无界增长。
         * @param filePath 发生变更的文件绝对路径
         * @return true 需要派发回调
         * @return false 仍在防抖窗口内，应当丢弃
         * @note 仅供监听线程调用：防抖表不做并发保护，各实现只在自己的读取线程内调用本方法
         */
        [[nodiscard]] bool shouldDispatchChange(const std::string &filePath);

        std::chrono::milliseconds                                              m_debounceInterval{100}; ///< 防抖间隔毫秒数
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_lastEventTime;         ///< 各路径上次触发的事件时间

    private:
        /// 防抖时间戳表最多跟踪的路径数，超出后清理已过期记录
        static constexpr std::size_t kMaximumDebouncedPaths = 4096;
    };
} // namespace AsynGyanis::Platform
