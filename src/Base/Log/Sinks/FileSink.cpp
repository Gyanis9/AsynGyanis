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
#if ASYN_PLATFORM_WIN32
        // 整段圈进条件编译：GCC 把匿名命名空间里没人调用的函数按 -Wunused-function 报出来，
        // 而 Linux 侧门禁带 -Werror（MSVC 不报这条，只有容器那一侧看得见）
        /**
         * @brief 就地把串里每个 `'\n'` 换成 `"\r\n"`
         * @details Windows 上由我们自己补行尾，好把文件按二进制打开：文本模式的流逐字符走换行翻译，
         *          实测每行多付约 110 纳秒，而两种写法落盘字节逐字相同（含每帧带换行的调用栈正文）。
         *          绝大多数行只有行尾那一个换行，改末位字符即可；带栈的行才整段展开。
         * @param line 待补齐的一行（已含行尾换行）
         */
        void translateNewlinesToCrLf(std::string &line)
        {
            const std::size_t newlineCount = static_cast<std::size_t>(std::ranges::count(line, '\n'));
            if (newlineCount == 0)
            {
                return;
            }
            if (newlineCount == 1 && line.back() == '\n')
            {
                line.back() = '\r';
                line.push_back('\n');
                return;
            }

            std::string expanded;
            expanded.reserve(line.size() + newlineCount);
            std::size_t segmentStart = 0;
            for (std::size_t position = 0; position < line.size(); ++position)
            {
                if (line[position] != '\n')
                {
                    continue;
                }
                expanded.append(line, segmentStart, position - segmentStart);
                expanded.append("\r\n");
                segmentStart = position + 1;
            }
            expanded.append(line, segmentStart, std::string::npos);
            line.swap(expanded);
        }
#endif

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

        // 二进制打开 + 自己补行尾（见 translateNewlinesToCrLf）：文本模式的翻译路径逐字符走，
        // 每行多付约 110 纳秒。POSIX 上 binary 标志不改变任何行为
        auto mode = std::ios::out | std::ios::binary;
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
        // 合并后只有一轮。Windows 上自己把换行补成 "\r\n"（流已按二进制打开），落盘字节与
        // 文本模式逐字一致
        m_lineBuffer.push_back('\n');
#if ASYN_PLATFORM_WIN32
        translateNewlinesToCrLf(m_lineBuffer);
#endif
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

        // 报回真正落到磁盘上的字节数：Windows 的行尾由我们自己补成 "\r\n"，缓冲里的长度就是落盘长度，
        // 不必再按换行个数补差。按大小滚动的阈值直接累加这个数（见 RollingFileSink::write），
        // 报小了活动文件就会系统性超出上限才滚
        return m_lineBuffer.size();
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
        m_file.open(m_filePath, std::ios::out | std::ios::app | std::ios::binary);
    }
} // namespace AsynGyanis::Base
