#include "ConfigFileWatcher.h"

#include <algorithm>
#include <filesystem>
#include <poll.h>


namespace Base
{
    std::unique_ptr<IFileWatcher> FileWatcherFactory::create()
    {
        return std::make_unique<InotifyFileWatcher>();
    }

    InotifyFileWatcher::InotifyFileWatcher()
    {
        m_inotify_fd = inotify_init1(IN_CLOEXEC);
        if (m_inotify_fd < 0)
        {
            throw std::runtime_error("Failed to initialize inotify");
        }
    }

    InotifyFileWatcher::~InotifyFileWatcher()
    {
        stop();
        if (m_inotify_fd >= 0)
        {
            close(m_inotify_fd);
        }
    }

    bool InotifyFileWatcher::start()
    {
        if (m_running.load(std::memory_order_acquire))
        {
            return true;
        }

        if (m_inotify_fd < 0)
        {
            return false;
        }

        m_should_stop.store(false, std::memory_order_release);

        try
        {
            m_watch_thread = std::make_unique<std::thread>(&InotifyFileWatcher::watchLoop, this);
        }
        catch (const std::system_error &)
        {
            return false;
        }

        m_running.store(true, std::memory_order_release);

        return true;
    }

    void InotifyFileWatcher::stop()
    {
        if (!m_running.load(std::memory_order_acquire))
        {
            return;
        }

        m_should_stop.store(true, std::memory_order_release);

        if (m_watch_thread && m_watch_thread->joinable())
        {
            m_watch_thread->join();
        }

        m_watch_thread.reset();
        m_running.store(false, std::memory_order_release);
    }

    bool InotifyFileWatcher::addWatch(const std::string_view path, const bool recursive)
    {
        std::error_code ec;

        const auto abs_path = std::filesystem::absolute(path, ec).string();
        if (ec)
        {
            return false;
        }

        {
            std::lock_guard lock(m_watch_mutex);

            // 检查是否已经在监听
            if (m_path_to_wd.contains(abs_path))
            {
                return true;
            }

            const int wd = inotify_add_watch(m_inotify_fd, abs_path.c_str(), WATCH_MASK);
            if (wd < 0)
            {
                return false;
            }

            m_watch_descriptors[wd] = abs_path;
            m_path_to_wd[abs_path]  = wd;
        }

        // 递归监听子目录（不在锁内调用以免死锁或长时间持锁）
        if (recursive && std::filesystem::is_directory(abs_path, ec))
        {
            for (const auto &entry: std::filesystem::recursive_directory_iterator(abs_path, ec))
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

    bool InotifyFileWatcher::removeWatch(const std::string_view path)
    {
        std::error_code ec;

        const auto abs_path = std::filesystem::absolute(path, ec).string();
        if (ec)
        {
            return false;
        }

        std::lock_guard lock(m_watch_mutex);

        const auto it = m_path_to_wd.find(abs_path);
        if (it == m_path_to_wd.end())
        {
            return false;
        }

        const int wd = it->second;
        // 返回值检查：失败时（如 EINVAL）仍从映射中移除，防止悬挂引用
        [[maybe_unused]] auto _ = inotify_rm_watch(m_inotify_fd, wd);

        m_watch_descriptors.erase(wd);
        m_path_to_wd.erase(it);

        return true;
    }

    void InotifyFileWatcher::setCallback(FileChangeCallback callback)
    {
        std::lock_guard lock(m_watch_mutex);
        m_callback = std::move(callback);
    }

    bool InotifyFileWatcher::isRunning() const noexcept
    {
        return m_running.load(std::memory_order_acquire);
    }

    void InotifyFileWatcher::setDebounceInterval(const std::chrono::milliseconds interval) noexcept
    {
        m_debounce_interval = interval;
    }

    void InotifyFileWatcher::watchLoop()
    {
        while (!m_should_stop.load(std::memory_order_acquire))
        {
            pollfd pfd;
            pfd.fd     = m_inotify_fd;
            pfd.events = POLLIN;

            const int ret = poll(&pfd, 1, 100);

            if (ret < 0)
            {
                if (errno == EINTR)
                    continue;
                break;
            }

            if (ret == 0)
                continue;

            if (pfd.revents & POLLIN)
            {
                processEvents();
            }
        }
    }

    std::string InotifyFileWatcher::getEventDescription(const inotify_event *event)
    {
        std::string desc;
        if (event->mask & IN_CLOSE_WRITE)
            desc += "CLOSE_WRITE ";
        if (event->mask & IN_MOVED_TO)
            desc += "MOVED_TO ";
        if (event->mask & IN_DELETE_SELF)
            desc += "DELETE_SELF ";
        if (event->mask & IN_MOVE_SELF)
            desc += "MOVE_SELF ";
        if (event->mask & IN_ISDIR)
            desc += "ISDIR ";
        if (desc.empty())
            desc = "UNKNOWN";
        desc += ": ";
        desc += event->name;
        return desc;
    }

    void InotifyFileWatcher::processEvents()
    {
        char buffer[EVENT_BUFFER_SIZE];

        const ssize_t len = read(m_inotify_fd, buffer, sizeof(buffer));
        if (len < 0)
        {
            return;
        }

        ssize_t i = 0;
        while (i < len)
        {
            const auto event = reinterpret_cast<const inotify_event *>(&buffer[i]);

            if (event->len > 0)
            {
                // 在锁内提取回调相关的数据快照，锁外调用回调以避免死锁
                FileChangeCallback callbackCopy;
                std::string        full_path;
                FileChangeEvent    evt = FileChangeEvent::Modified;

                {
                    std::shared_lock lock(m_watch_mutex);

                    if (auto wd_it = m_watch_descriptors.find(event->wd); wd_it != m_watch_descriptors.end())
                    {
                        full_path = wd_it->second;
                        if (!full_path.empty() && full_path.back() != '/')
                        {
                            full_path += '/';
                        }
                        full_path += event->name;

                        auto now = std::chrono::steady_clock::now();
                        if (auto last_it = m_last_event_time.find(full_path); last_it != m_last_event_time.end())
                        {
                            if (now - last_it->second < m_debounce_interval)
                            {
                                i += sizeof(inotify_event) + event->len;
                                continue;
                            }
                        }
                        m_last_event_time[full_path] = now;

                        callbackCopy = m_callback;

                        if (event->mask & IN_CLOSE_WRITE)
                        {
                            evt = FileChangeEvent::Modified;
                        } else if (event->mask & IN_MOVED_TO)
                        {
                            evt = FileChangeEvent::Created;
                        } else if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF))
                        {
                            evt = FileChangeEvent::Deleted;
                        }
                    }
                }

                // 锁外调用回调，防止回调中修改 watcher 引起死锁
                if (callbackCopy)
                {
                    callbackCopy(full_path, evt);
                }
            }

            i += sizeof(inotify_event) + event->len;
        }
    }
}
