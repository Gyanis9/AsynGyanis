/**
 * @file Win32FileWatcher.cpp
 * @brief Windows 平台文件监听器，基于 ReadDirectoryChangesW 重叠 IO
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Platform/FileSystem/Win32FileWatcher.h"

#include "Platform/System/TextEncoding.h"

#include <filesystem>
#include <ranges>
#include <system_error>

namespace AsynGyanis::Platform
{
    namespace
    {
        /**
         * @brief 把 Windows 目录通知动作映射为平台无关的变更类型
         * @param action FILE_ACTION_* 常量
         * @return FileChangeType 映射后的变更类型
         */
        FileChangeType changeTypeFromAction(const DWORD action)
        {
            switch (action)
            {
                case FILE_ACTION_ADDED:
                    return FileChangeType::Created;
                case FILE_ACTION_REMOVED:
                    return FileChangeType::Deleted;
                case FILE_ACTION_RENAMED_OLD_NAME:
                    return FileChangeType::Moved;
                case FILE_ACTION_MODIFIED:
                case FILE_ACTION_RENAMED_NEW_NAME:
                default:
                    return FileChangeType::Modified;
            }
        }
    } // namespace

    Win32FileWatcher::Win32FileWatcher()
    {
        m_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    Win32FileWatcher::~Win32FileWatcher()
    {
        Win32FileWatcher::stop();

        if (m_stopEvent != nullptr)
        {
            ::CloseHandle(m_stopEvent);
            m_stopEvent = nullptr;
        }
    }

    bool Win32FileWatcher::start()
    {
        if (m_running.load(std::memory_order_acquire))
        {
            return true;
        }

        if (m_stopEvent == nullptr)
        {
            return false;
        }

        m_shouldStop.store(false, std::memory_order_release);
        ::ResetEvent(m_stopEvent);

        try
        {
            // jthread 直接作为成员启动，无需堆分配；循环靠 m_shouldStop 与停止事件退出
            m_watchThread = std::jthread([this]
            {
                watchLoop();
            });
        } catch (const std::system_error &)
        {
            return false;
        }

        m_running.store(true, std::memory_order_release);
        return true;
    }

    void Win32FileWatcher::stop()
    {
        m_shouldStop.store(true, std::memory_order_release);
        if (m_stopEvent != nullptr)
        {
            ::SetEvent(m_stopEvent);
        }

        if (m_watchThread.joinable())
        {
            // 赋默认构造的 jthread 会先 request_stop 再 join，阻塞式循环已因停止事件自行退出
            m_watchThread = std::jthread{};
        }

        {
            std::lock_guard lock(m_watchMutex);
            for (auto &entry: m_watches | std::views::values)
            {
                closeEntry(*entry);
            }
            m_watches.clear();
        }

        m_running.store(false, std::memory_order_release);
    }

    bool Win32FileWatcher::addWatch(const std::string_view path, const bool recursive)
    {
        std::error_code errorCode;
        const auto      absolutePath = std::filesystem::absolute(path, errorCode).string();
        if (errorCode)
        {
            return false;
        }

        {
            const std::string directoryPath = normalizeDirectoryPath(absolutePath);
            std::lock_guard   lock(m_watchMutex);

            if (m_watches.contains(directoryPath))
            {
                return true;
            }

            auto entry  = std::make_unique<WatchEntry>();
            entry->path = directoryPath;
            entry->buffer.resize(kBufferSize);
            entry->directoryHandle = ::CreateFileW(TextEncoding::toWideString(absolutePath).c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);

            if (entry->directoryHandle == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            entry->eventHandle = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (entry->eventHandle == nullptr)
            {
                ::CloseHandle(entry->directoryHandle);
                entry->directoryHandle = INVALID_HANDLE_VALUE;
                return false;
            }

            entry->overlapped.hEvent = entry->eventHandle;
            issueRead(*entry);

            m_watches[directoryPath] = std::move(entry);
        }

        // 递归注册放在锁外，避免持锁期间遍历目录树
        if (recursive && std::filesystem::is_directory(absolutePath, errorCode))
        {
            for (const auto &directoryEntry: std::filesystem::recursive_directory_iterator(absolutePath, errorCode))
            {
                if (errorCode)
                {
                    break;
                }
                if (directoryEntry.is_directory())
                {
                    addWatch(directoryEntry.path().string(), false);
                }
            }
        }

        return true;
    }

    bool Win32FileWatcher::removeWatch(const std::string_view path)
    {
        std::error_code errorCode;
        const auto      absolutePath = std::filesystem::absolute(path, errorCode).string();
        if (errorCode)
        {
            return false;
        }

        std::lock_guard lock(m_watchMutex);

        const auto iterator = m_watches.find(normalizeDirectoryPath(absolutePath));
        if (iterator == m_watches.end())
        {
            return false;
        }

        closeEntry(*iterator->second);
        m_watches.erase(iterator);
        return true;
    }

    void Win32FileWatcher::setCallback(FileChangeCallback callback)
    {
        std::lock_guard lock(m_watchMutex);
        m_callback = std::move(callback);
    }

    bool Win32FileWatcher::isRunning() const noexcept
    {
        return m_running.load(std::memory_order_acquire);
    }

    void Win32FileWatcher::watchLoop()
    {
        // 提到循环外并预留到系统上限：clear() 不回收容量，之后每轮收集不再产生堆分配
        std::vector<HANDLE>      eventHandles;
        std::vector<std::string> pendingPaths;
        eventHandles.reserve(MAXIMUM_WAIT_OBJECTS);
        pendingPaths.reserve(MAXIMUM_WAIT_OBJECTS - 1);

        while (!m_shouldStop.load(std::memory_order_acquire))
        {
            // 收集所有待等待的事件句柄及其对应监听路径，停止事件固定占据索引 0
            eventHandles.clear();
            pendingPaths.clear();

            eventHandles.push_back(m_stopEvent);

            {
                std::shared_lock lock(m_watchMutex);
                for (const auto &[path, entry]: m_watches)
                {
                    if (entry->pending)
                    {
                        eventHandles.push_back(entry->eventHandle);
                        pendingPaths.push_back(path);
                    }
                }
            }

            // WaitForMultipleObjects 单次最多等待 MAXIMUM_WAIT_OBJECTS 个对象，超出部分留待下轮
            if (eventHandles.size() > MAXIMUM_WAIT_OBJECTS)
            {
                eventHandles.resize(MAXIMUM_WAIT_OBJECTS);
                pendingPaths.resize(MAXIMUM_WAIT_OBJECTS - 1);
            }

            if (eventHandles.size() <= 1)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            const DWORD waitResult = ::WaitForMultipleObjects(static_cast<DWORD>(eventHandles.size()), eventHandles.data(), FALSE, 100);

            if (waitResult == WAIT_FAILED || waitResult == WAIT_TIMEOUT)
            {
                continue;
            }

            const DWORD index = waitResult - WAIT_OBJECT_0;
            if (index == 0)
            {
                break; // 停止事件被置位
            }

            if (index - 1 >= pendingPaths.size())
            {
                continue;
            }

            const std::string                                    targetPath = pendingPaths[index - 1];
            std::vector<std::pair<std::string, FileChangeType> > pendingCallbacks;
            FileChangeCallback                                   callbackSnapshot;
            {
                std::shared_lock lock(m_watchMutex);

                const auto iterator = m_watches.find(targetPath);
                if (iterator == m_watches.end())
                {
                    continue; // 监听已在本轮等待期间被移除
                }

                WatchEntry &entry = *iterator->second;
                processEntry(entry, pendingCallbacks);
                callbackSnapshot = m_callback;

                // 处理完毕后重新发起下一次重叠读
                ::ResetEvent(entry.eventHandle);
                entry.pending = false;
                issueRead(entry);
            }

            // 锁外批量触发回调，防止回调中增删监听路径造成死锁
            if (callbackSnapshot)
            {
                for (const auto &[changedPath, changeType]: pendingCallbacks)
                {
                    callbackSnapshot(changedPath, changeType);
                }
            }
        }
    }

    void Win32FileWatcher::processEntry(WatchEntry &entry, std::vector<std::pair<std::string, FileChangeType> > &events)
    {
        DWORD bytesTransferred = 0;
        if (!::GetOverlappedResult(entry.directoryHandle, &entry.overlapped, &bytesTransferred, FALSE))
        {
            return;
        }

        if (bytesTransferred == 0)
        {
            return;
        }

        const auto *information = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(entry.buffer.data());
        while (true)
        {
            const std::wstring wideName(information->FileName, information->FileNameLength / sizeof(wchar_t));
            const std::string  fileName = TextEncoding::toUtf8String(wideName);

            std::string fullPath;
            fullPath.reserve(entry.path.size() + fileName.size());
            fullPath.append(entry.path);
            fullPath.append(fileName);

            const FileChangeType changeType = changeTypeFromAction(information->Action);

            // 防抖与过期记录清理由基类统一实现，仅监听线程调用
            if (shouldDispatchChange(fullPath))
            {
                events.emplace_back(std::move(fullPath), changeType);
            }

            if (information->NextEntryOffset == 0)
            {
                break;
            }
            information = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(reinterpret_cast<const uint8_t *>(information) + information->NextEntryOffset);
        }
    }

    void Win32FileWatcher::issueRead(WatchEntry &entry) const
    {
        entry.overlapped        = OVERLAPPED{};
        entry.overlapped.hEvent = entry.eventHandle;
        entry.pending           = true;

        DWORD      bytesReturned = 0;
        const BOOL success       = ::ReadDirectoryChangesW(entry.directoryHandle, entry.buffer.data(), static_cast<DWORD>(entry.buffer.size()), FALSE, kWatchFilter, &bytesReturned,
                                                     &entry.overlapped, nullptr);

        if (!success)
        {
            entry.pending = false;
        }
    }

    void Win32FileWatcher::closeEntry(WatchEntry &entry) const
    {
        if (entry.pending && entry.directoryHandle != INVALID_HANDLE_VALUE)
        {
            ::CancelIo(entry.directoryHandle);
            DWORD bytesTransferred = 0;
            ::GetOverlappedResult(entry.directoryHandle, &entry.overlapped, &bytesTransferred, TRUE);
        }
        if (entry.eventHandle != nullptr)
        {
            ::CloseHandle(entry.eventHandle);
            entry.eventHandle = nullptr;
        }
        if (entry.directoryHandle != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(entry.directoryHandle);
            entry.directoryHandle = INVALID_HANDLE_VALUE;
        }
        entry.pending = false;
    }

    std::string Win32FileWatcher::normalizeDirectoryPath(const std::string &path)
    {
        // ReadDirectoryChangesW 报告的文件名相对于被监听目录，故目录路径须以反斜杠结尾
        if (path.empty() || path.back() == '\\')
        {
            return path;
        }
        return path + '\\';
    }
} // namespace AsynGyanis::Platform
