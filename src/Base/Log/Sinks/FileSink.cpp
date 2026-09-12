#include "Base/Log/Sinks/FileSink.h"

#include <filesystem>
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
        static_cast<void>(writeLine(formatEvent(event)));
    }

    std::size_t FileSink::writeLine(const std::string_view line)
    {
        std::lock_guard lock(m_mutex);
        if (!m_file.is_open())
        {
            return 0;
        }
        // 换行并入复用的行缓冲后整行只做一次 <<：流插入每次都要构造 sentry 并由文件
        // 缓冲加锁，合并后只有一轮；复用成员缓冲让拼接不产生新分配，落盘的字节流不变
        m_lineBuffer.assign(line);
        m_lineBuffer.push_back('\n');
        m_file << m_lineBuffer;
        // 返回写入字节数（换行按 1 字节计）：文本模式下 Windows 会额外补 '\r'，
        // 调用方只用它做「是否达到滚动阈值」的近似判据，不需要与磁盘大小逐字节相等
        return line.size() + 1;
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
        // 同样使用 error_code 重载：reopen 常在运行期由滚动/切换路径调用，
        // 这里不允许抛异常打断日志写入；若目录无法创建，随后的 open 会失败，
        // 文件保持关闭状态（write() 对已关闭文件静默跳过）
        static_cast<void>(createParentDirectory(m_filePath.parent_path()));
        m_file.open(m_filePath, std::ios::out | std::ios::app);
    }
} // namespace AsynGyanis::Base
