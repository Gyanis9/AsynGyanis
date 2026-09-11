#include "Base/Log/Sinks/FileSink.h"

#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace AsynGyanis::Base
{
    FileSink::FileSink(std::filesystem::path filePath, const bool truncate) :
        m_filePath(std::move(filePath))
    {
        if (const auto parent = m_filePath.parent_path(); !parent.empty())
        {
            std::filesystem::create_directories(parent);
        }
        auto mode = std::ios::out;
        if (truncate)
        {
            mode |= std::ios::trunc;
        } else
        {
            mode |= std::ios::app;
        }
        m_file.open(m_filePath, mode);
        if (!m_file.is_open())
        {
            throw std::runtime_error("Failed to open log file: " + m_filePath.string());
        }
    }

    FileSink::~FileSink()
    {
        FileSink::flush();
    }

    void FileSink::write(const LogEvent &event)
    {
        std::lock_guard lock(m_mutex);
        if (m_file.is_open())
        {
            m_file << formatEvent(event) << '\n';
        }
    }

    void FileSink::flush()
    {
        std::lock_guard lock(m_mutex);
        if (m_file.is_open())
        {
            m_file.flush();
        }
    }

    void FileSink::reopen(const std::filesystem::path &newPath)
    {
        std::lock_guard lock(m_mutex);
        m_file.close();
        m_filePath = newPath;
        if (const auto parent = m_filePath.parent_path(); !parent.empty())
        {
            std::filesystem::create_directories(parent);
        }
        m_file.open(m_filePath, std::ios::out | std::ios::app);
    }
} // namespace AsynGyanis::Base
