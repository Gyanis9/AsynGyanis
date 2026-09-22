#include "Base/Log/Sinks/FileSink.h"
#include "Platform/Platform.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 创建缺失的父目录，失败时返回中文原因串
         * @details 一律走 std::filesystem 的 error_code 重载：日志路径上不接受
         *          std::filesystem_error 直接逃逸（写日志失败不应影响业务流程），
         *          这里把失败转成可拼进异常文本的描述，由调用方决定如何处置。
         * @param parent 父目录路径
         * @return std::string 成功返回空串，失败返回 "（创建目录失败：<原因>）"
         */
        [[nodiscard]] std::string createParentDirectory(const std::filesystem::path &parent)
        {
            if (parent.empty())
            {
                return {};
            }

            // create_directories 在目录已存在时返回 false 且不置错误码，因此必须同时判错误码
            if (std::error_code errorCode; !std::filesystem::create_directories(parent, errorCode) && errorCode)
            {
                return "（创建目录失败：" + errorCode.message() + "）";
            }
            return {};
        }
    } // namespace

    FileSink::FileSink(std::filesystem::path filePath, const bool truncate) :
        m_filePath(std::move(filePath))
    {
        const std::string directoryError = createParentDirectory(m_filePath.parent_path());

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
            throw std::runtime_error("无法打开日志文件：" + m_filePath.string() + directoryError);
        }
    }

    FileSink::~FileSink()
    {
        FileSink::flush();
    }

    void FileSink::write(const LogEvent &event)
    {
        const std::lock_guard lock(m_mutex);
        // 版式直接落在本 Sink 留了容量的行缓冲上：std::format 交出结果串本身要取两次堆，
        // 而这里连换行都续在同一个缓冲里，稳态下拼一行不碰堆
        m_lineBuffer.clear();
        formatEventInto(m_lineBuffer, event);
        static_cast<void>(writePreparedLineLocked());
    }

    std::size_t FileSink::writeLine(const std::string_view line)
    {
        const std::lock_guard lock(m_mutex);
        m_lineBuffer.assign(line);
        return writePreparedLineLocked();
    }

    std::size_t FileSink::writePreparedLineLocked()
    {
        if (!m_file.is_open())
        {
            return 0;
        }
        // 换行并入缓冲后整行只做一次 <<：流插入每次都要构造 sentry 并由文件缓冲加锁，
        // 合并后只有一轮，落盘的字节流与「正文 + \n」逐字一致
        m_lineBuffer.push_back('\n');
        m_file << m_lineBuffer;

        // 写入后必须看流状态：磁盘写满或配额耗尽时插入不会抛异常，只会把 failbit/badbit 置起，
        // 此后每次 << 都是空操作——日志整片静默消失，而返回值还在报「写成功了」。
        // 这里如实返回 0，并把诊断写到标准错误：日志系统自身出了故障，没有别的去处可报。
        // 诊断只在「连续失败」的第一条上报一次，避免磁盘故障时每一行日志都去写一次标准错误
        if (!m_file.good())
        {
            if (!m_hasReportedWriteFailure)
            {
                m_hasReportedWriteFailure = true;
                std::cerr << "FileSink：写日志失败（磁盘写满或配额耗尽）：" << m_filePath.string()
                        << "；流已失效，后续日志不会再落盘，重新打开该文件（reopen）后恢复" << '\n';
            }
            return 0;
        }
        m_hasReportedWriteFailure = false;

        // 返回落到磁盘上的真实字节数：Windows 的文本模式会把每个 '\n' 翻成 "\r\n"，
        // 而带调用栈的行每帧还有一个 '\n'。按大小滚动的阈值直接累加这个数（见
        // RollingFileSink::write），报小了活动文件就会系统性超出上限才滚
        std::size_t landedByteCount = m_lineBuffer.size();
#if ASYN_PLATFORM_WIN32
        landedByteCount += static_cast<std::size_t>(std::ranges::count(m_lineBuffer, '\n'));
#endif
        return landedByteCount;
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
        // 换了一条流，上一个流上的失败不该压着新的：下一次再失败要重新报一次
        m_hasReportedWriteFailure = false;
        // 同样使用 error_code 重载：reopen 常在运行期由滚动/切换路径调用，
        // 这里不允许抛异常打断日志写入；若目录无法创建，随后的 open 会失败，
        // 文件保持关闭状态（write() 对已关闭文件静默跳过）
        static_cast<void>(createParentDirectory(m_filePath.parent_path()));
        m_file.open(m_filePath, std::ios::out | std::ios::app);
    }
} // namespace AsynGyanis::Base
