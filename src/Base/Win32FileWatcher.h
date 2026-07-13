/**
 * @file Win32FileWatcher.h
 * @brief Windows 平台文件监听器（基于 ReadDirectoryChangesW）
 * @copyright Copyright (c) 2026
 */

#ifndef BASE_WIN32FILEWATCHER_H
#define BASE_WIN32FILEWATCHER_H

#include "ConfigFileWatcher.h"

#include "Platform/Platform.h"

#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace Base
{
    /**
     * @brief Windows 平台文件监听器
     *
     * 使用 ReadDirectoryChangesW + overlapped I/O 监听文件变更。
     * 特性：
     *   - 监听文件修改、创建、删除、重命名
     *   - 独立监听线程，不阻塞主线程
     *   - 支持防抖，避免短时间内重复触发
     */
    class Win32FileWatcher : public IFileWatcher
    {
    public:
        Win32FileWatcher();
        ~Win32FileWatcher() override;

        Win32FileWatcher(const Win32FileWatcher &)            = delete;
        Win32FileWatcher &operator=(const Win32FileWatcher &) = delete;
        Win32FileWatcher(Win32FileWatcher &&)                 = delete;
        Win32FileWatcher &operator=(Win32FileWatcher &&)      = delete;

        bool start() override;
        void stop() override;
        bool addWatch(std::string_view path, bool recursive = false) override;
        bool removeWatch(std::string_view path) override;
        void setCallback(FileChangeCallback callback) override;
        [[nodiscard]] bool isRunning() const noexcept override;
        void setDebounceInterval(std::chrono::milliseconds interval) noexcept override;

    private:
        /**
         * @brief 每个监听目录的上下文
         */
        struct WatchEntry
        {
            HANDLE                   hDir{INVALID_HANDLE_VALUE};
            HANDLE                   hEvent{nullptr};
            std::string              path;
            std::vector<uint8_t>     buffer;
            OVERLAPPED               overlapped{};
            bool                     pending{false};
        };

        void watchLoop();
        void processEntry(WatchEntry &entry);
        void issueRead(WatchEntry &entry);
        void closeEntry(WatchEntry &entry);

        std::unordered_map<std::string, std::unique_ptr<WatchEntry>> m_watches;

        FileChangeCallback           m_callback;
        mutable std::shared_mutex    m_watch_mutex;
        std::unique_ptr<std::thread> m_watch_thread;
        std::atomic<bool>            m_running{false};
        std::atomic<bool>            m_should_stop{false};
        HANDLE                       m_stopEvent{nullptr};

        std::chrono::milliseconds                                              m_debounce_interval{100};
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_last_event_time;

        static constexpr size_t BUFFER_SIZE = 4096;
        static constexpr DWORD  WATCH_FILTER =
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
    };
}

#endif // BASE_WIN32FILEWATCHER_H
