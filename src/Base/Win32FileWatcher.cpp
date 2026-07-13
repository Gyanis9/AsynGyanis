/**
 * @file Win32FileWatcher.cpp
 * @brief Windows 平台文件监听器实现
 * @copyright Copyright (c) 2026
 */

#include "Win32FileWatcher.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <thread>

namespace Base
{
    Win32FileWatcher::Win32FileWatcher()
    {
        m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    Win32FileWatcher::~Win32FileWatcher()
    {
        stop();
        if (m_stopEvent)
        {
            CloseHandle(m_stopEvent);
            m_stopEvent = nullptr;
        }
    }

    bool Win32FileWatcher::start()
    {
        if (m_running.load(std::memory_order_acquire))
        {
            return true;
        }

        m_should_stop.store(false, std::memory_order_release);
        ResetEvent(m_stopEvent);

        try
        {
            m_watch_thread = std::make_unique<std::thread>(&Win32FileWatcher::watchLoop, this);
        } catch (const std::system_error &)
        {
            return false;
        }

        m_running.store(true, std::memory_order_release);
        return true;
    }

    void Win32FileWatcher::stop()
    {
        if (!m_running.load(std::memory_order_acquire))
        {
            return;
        }

        m_should_stop.store(true, std::memory_order_release);
        SetEvent(m_stopEvent);

        if (m_watch_thread && m_watch_thread->joinable())
        {
            m_watch_thread->join();
        }

        m_watch_thread.reset();
        m_running.store(false, std::memory_order_release);
    }

    bool Win32FileWatcher::addWatch(const std::string_view path, const bool recursive)
    {
        std::error_code ec;
        const auto      abs_path = std::filesystem::absolute(path, ec).string();
        if (ec)
        {
            return false;
        }

        // 确保路径以反斜杠结尾（ReadDirectoryChangesW 报告的文件名是相对于目录的）
        std::string dirPath = abs_path;
        if (!dirPath.empty() && dirPath.back() != '\\')
        {
            dirPath += '\\';
        }

        {
            std::lock_guard lock(m_watch_mutex);

            if (m_watches.contains(dirPath))
            {
                return true;
            }

            auto entry    = std::make_unique<WatchEntry>();
            entry->path   = dirPath;
            entry->buffer.resize(BUFFER_SIZE);
            entry->hDir   = CreateFileW(
                std::wstring(abs_path.begin(), abs_path.end()).c_str(),
                FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                nullptr);

            if (entry->hDir == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            entry->hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!entry->hEvent)
            {
                CloseHandle(entry->hDir);
                return false;
            }

            entry->overlapped.hEvent = entry->hEvent;
            issueRead(*entry);

            m_watches[dirPath] = std::move(entry);
        }

        // 递归监听子目录
        if (recursive && std::filesystem::is_directory(abs_path, ec))
        {
            for (const auto &entry : std::filesystem::recursive_directory_iterator(abs_path, ec))
            {
                if (ec)
                    break;
                if (entry.is_directory())
                {
                    addWatch(entry.path().string(), false);
                }
            }
        }

        return true;
    }

    bool Win32FileWatcher::removeWatch(const std::string_view path)
    {
        std::error_code ec;
        const auto      abs_path = std::filesystem::absolute(path, ec).string();
        if (ec)
        {
            return false;
        }

        std::string dirPath = abs_path;
        if (!dirPath.empty() && dirPath.back() != '\\')
        {
            dirPath += '\\';
        }

        std::lock_guard lock(m_watch_mutex);

        const auto it = m_watches.find(dirPath);
        if (it == m_watches.end())
        {
            return false;
        }

        closeEntry(*it->second);
        m_watches.erase(it);
        return true;
    }

    void Win32FileWatcher::setCallback(FileChangeCallback callback)
    {
        std::lock_guard lock(m_watch_mutex);
        m_callback = std::move(callback);
    }

    bool Win32FileWatcher::isRunning() const noexcept
    {
        return m_running.load(std::memory_order_acquire);
    }

    void Win32FileWatcher::setDebounceInterval(const std::chrono::milliseconds interval) noexcept
    {
        m_debounce_interval = interval;
    }

    void Win32FileWatcher::watchLoop()
    {
        while (!m_should_stop.load(std::memory_order_acquire))
        {
            // 收集所有事件句柄
            std::vector<HANDLE> events;
            std::vector<WatchEntry *> entries;

            events.push_back(m_stopEvent);

            {
                std::shared_lock lock(m_watch_mutex);
                for (auto &[path, entry] : m_watches)
                {
                    if (entry->pending)
                    {
                        events.push_back(entry->hEvent);
                        entries.push_back(entry.get());
                    }
                }
            }

            if (events.size() <= 1)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            const DWORD result = WaitForMultipleObjects(
                static_cast<DWORD>(events.size()), events.data(), FALSE, 100);

            if (result == WAIT_FAILED || result == WAIT_TIMEOUT)
            {
                continue;
            }

            const DWORD index = result - WAIT_OBJECT_0;
            if (index == 0)
            {
                break; // stop event
            }

            if (index >= entries.size() + 1)
            {
                continue;
            }

            auto *entry = entries[index - 1];
            processEntry(*entry);

            // 重新发起读取
            ResetEvent(entry->hEvent);
            entry->pending = false;

            {
                std::shared_lock lock(m_watch_mutex);
                issueRead(*entry);
            }
        }
    }

    void Win32FileWatcher::processEntry(WatchEntry &entry)
    {
        DWORD bytesTransferred = 0;
        if (!GetOverlappedResult(entry.hDir, &entry.overlapped, &bytesTransferred, FALSE))
        {
            return;
        }

        if (bytesTransferred == 0)
        {
            return;
        }

        auto *info = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(entry.buffer.data());

        while (true)
        {
            // 转换宽字符文件名为 UTF-8 多字节字符串
            const int nameLen = info->FileNameLength / sizeof(wchar_t);
            std::wstring wname(info->FileName, nameLen);
            std::string filename;
            int convLen = WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1,
                                              nullptr, 0, nullptr, nullptr);
            if (convLen > 0)
            {
                filename.resize(convLen - 1);
                WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1,
                                   filename.data(), convLen, nullptr, nullptr);
            }

            std::string fullPath = entry.path + filename;

            // 映射事件类型
            FileChangeEvent evt = FileChangeEvent::Modified;
            switch (info->Action)
            {
                case FILE_ACTION_ADDED:
                    evt = FileChangeEvent::Created;
                    break;
                case FILE_ACTION_REMOVED:
                    evt = FileChangeEvent::Deleted;
                    break;
                case FILE_ACTION_MODIFIED:
                case FILE_ACTION_RENAMED_NEW_NAME:
                    evt = FileChangeEvent::Modified;
                    break;
                case FILE_ACTION_RENAMED_OLD_NAME:
                    evt = FileChangeEvent::Moved;
                    break;
            }

            // 防抖
            FileChangeCallback callbackCopy;
            {
                std::shared_lock lock(m_watch_mutex);
                auto now = std::chrono::steady_clock::now();
                if (auto lastIt = m_last_event_time.find(fullPath); lastIt != m_last_event_time.end())
                {
                    if (now - lastIt->second < m_debounce_interval)
                    {
                        goto next;
                    }
                }
                m_last_event_time[fullPath] = now;
                callbackCopy = m_callback;
            }

            if (callbackCopy)
            {
                callbackCopy(fullPath, evt);
            }

        next:
            if (info->NextEntryOffset == 0)
            {
                break;
            }
            info = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(
                reinterpret_cast<uint8_t *>(info) + info->NextEntryOffset);
        }
    }

    void Win32FileWatcher::issueRead(WatchEntry &entry)
    {
        entry.overlapped = OVERLAPPED{};
        entry.overlapped.hEvent = entry.hEvent;
        entry.pending = true;

        DWORD bytesReturned = 0;
        const BOOL success = ReadDirectoryChangesW(
            entry.hDir,
            entry.buffer.data(),
            static_cast<DWORD>(entry.buffer.size()),
            FALSE,
            WATCH_FILTER,
            &bytesReturned,
            &entry.overlapped,
            nullptr);

        if (!success)
        {
            entry.pending = false;
        }
    }

    void Win32FileWatcher::closeEntry(WatchEntry &entry)
    {
        if (entry.pending && entry.hDir != INVALID_HANDLE_VALUE)
        {
            CancelIo(entry.hDir);
            DWORD bytesTransferred = 0;
            GetOverlappedResult(entry.hDir, &entry.overlapped, &bytesTransferred, TRUE);
        }
        if (entry.hEvent)
        {
            CloseHandle(entry.hEvent);
            entry.hEvent = nullptr;
        }
        if (entry.hDir != INVALID_HANDLE_VALUE)
        {
            CloseHandle(entry.hDir);
            entry.hDir = INVALID_HANDLE_VALUE;
        }
        entry.pending = false;
    }

} // namespace Base
