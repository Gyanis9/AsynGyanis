#include "Base/Log/Sinks/FileSink.h"
#include "Base/Exception/SystemException.h"
#include "Platform/FileSystem/FileSystem.h"
#include "Platform/Platform.h"
#include "Platform/System/TextEncoding.h"

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

    FileSink::FileSink(std::filesystem::path filePath, const bool truncate, const bool immediateFlush) : m_filePath(std::move(filePath)), m_immediateFlush(immediateFlush)
    {
        const std::string directoryError = createParentDirectory(m_filePath.parent_path());

        // 二进制打开 + 由 Platform 按本平台约定补行尾：文本模式的翻译路径逐字符走，
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
            // 路径按 UTF-8 拼进异常文本：`path::string()` 走本地代码页，代码页外的字符会直接抛出，
            // 那时逃出去的是「无法转码」而不是真正的打开失败，定位信息反而丢了
            //
            // 取框架的 SystemException 而不是裸 std::runtime_error：这是一条会交给调用方的构造期
            // 失败，而框架对外承诺「一句 catch (const Base::Exception &) 兜住所有运行期错误」——
            // 裸 std::runtime_error 恰好从那个捕获面上漏出去。errno 由失败的 ofstream::open 置好，
            // 正是本类「无法显式取得错误码的 CRT/文件类调用」那条通道的适用场景
            throw Base::SystemException("无法打开日志文件：" + AsynGyanis::Platform::FileSystem::utf8FromPath(m_filePath) + directoryError);
        }
    }

    FileSink::FileSink(std::ostream &stream) : m_filePath("<injected>"), m_injectedStream(&stream), m_immediateFlush(false)
    {
    }

    FileSink::~FileSink()
    {
        FileSink::flush();
    }

    std::ostream &FileSink::targetStream() noexcept
    {
        return m_injectedStream != nullptr ? *m_injectedStream : m_file;
    }

    bool FileSink::isStreamOpen() const noexcept
    {
        return m_injectedStream != nullptr || m_file.is_open();
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
        if (!isStreamOpen())
        {
            // 文件没打开（重开失败）时整条 Sink 不写：这一笔要留下读数，
            // 否则「日志在偷偷少」只有人肉翻标准错误才发现得到
            m_skippedLineCount.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        // 换行并入缓冲后整行只做一次 <<：流插入每次都要构造 sentry 并由文件缓冲加锁，
        // 合并后只有一轮。行尾由 Platform 按本平台约定补（Windows 补 CRLF、POSIX 原样保留 LF），
        // 流始终按二进制打开，落盘字节与文本模式逐字一致
        m_lineBuffer.push_back('\n');
        Platform::TextEncoding::applyNativeLineEndings(m_lineBuffer);
        targetStream() << m_lineBuffer;

        // 逐行同步（配置装出来的 sink 默认走这一步，见构造参数）：不交出去的那半行采集端与
        // 崩溃后的现场都读不到。放在流状态检查之前，让 flush 期间的失败（设备满、配额耗尽）
        // 与写入失败走同一个判据与同一本丢行账，不再多一条只在一侧出声的路
        if (m_immediateFlush)
        {
            targetStream().flush();
        }

        // 写入后必须看流状态：磁盘写满或配额耗尽时插入不会抛异常，只会把 failbit/badbit 置起，
        // 此后每次 << 都是空操作——日志整片静默消失，而返回值还在报「写成功了」。
        // 这里如实返回 0，诊断交给 reportStreamFailureOnceLocked（连续失败只报第一条）
        reportStreamFailureOnceLocked();
        if (!targetStream().good())
        {
            m_skippedLineCount.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }

        // 报回真正落到磁盘上的字节数：Windows 的行尾由我们自己补成 "\r\n"，缓冲里的长度就是落盘长度，
        // 不必再按换行个数补差。按大小滚动的阈值直接累加这个数（见 RollingFileSink::write），
        // 报小了活动文件就会系统性超出上限才滚
        return m_lineBuffer.size();
    }

    void FileSink::reportStreamFailureOnceLocked()
    {
        if (targetStream().good())
        {
            // 恢复过一次成功写入就重新武装：下一次故障还要出声
            m_hasReportedWriteFailure = false;
            return;
        }
        if (m_hasReportedWriteFailure)
        {
            return;
        }
        // 日志系统自身出了故障，没有别的去处可报——拿根日志器报自己等于让 write() 递归
        m_hasReportedWriteFailure = true;
        std::cerr << "FileSink：写日志失败（磁盘写满或配额耗尽）：" << AsynGyanis::Platform::FileSystem::utf8FromPath(m_filePath)
                  << "；流已失效，后续日志不会再落盘，重新打开该文件（reopen）后恢复" << '\n';
    }

    void FileSink::flush()
    {
        std::lock_guard lock(m_mutex);
        if (isStreamOpen())
        {
            targetStream().flush();
            // 缓冲没满时 << 只在内存里追加，设备满、配额耗尽往往要到这一次同步才浮出来。
            // 刷完不回头看流状态，「Fatal 那条已经落盘」就等于没人核过：打完就 abort 的调用方
            // 丢了最后一条日志，而现场连一句诊断都没有
            reportStreamFailureOnceLocked();
        }
    }

    void FileSink::reopen(const std::filesystem::path &newPath)
    {
        std::lock_guard lock(m_mutex);
        if (m_injectedStream != nullptr)
        {
            // 注入流没有真实文件可换；只更新诊断目标，写/刷仍走调用方交来的那条流
            m_filePath = newPath;
            return;
        }
        m_file.close();
        m_filePath = newPath;
        // 换了一条流，上一个流上的失败不该压着新的：下一次再失败要重新报一次
        m_hasReportedWriteFailure = false;
        // 同样使用 error_code 重载：reopen 常在运行期由滚动/切换路径调用，
        // 这里不允许抛异常打断日志写入；若目录无法创建，随后的 open 会失败，
        // 文件保持关闭状态（write() 对已关闭文件静默跳过）
        static_cast<void>(createParentDirectory(m_filePath.parent_path()));
        m_file.open(m_filePath, std::ios::out | std::ios::app | std::ios::binary);
        if (m_file.is_open())
        {
            // 重开成功：把上升沿重新武装，下一次失败还要再出声
            m_hasReportedReopenFailure = false;
            return;
        }
        reportReopenFailureOnceLocked();
    }

    std::size_t FileSink::skippedLineCount() const noexcept
    {
        return m_skippedLineCount.load(std::memory_order_relaxed);
    }

    void FileSink::reportReopenFailureOnceLocked()
    {
        if (m_hasReportedReopenFailure)
        {
            return;
        }
        // 与写失败那条分开是两条独立判据：文件没打开时流没有状态可判，writePreparedLineLocked 在判空处
        // 就早退，永远轮不到那条上报——不打这一行，整条 Sink 就是从这一刻起静默失聪
        m_hasReportedReopenFailure = true;
        std::cerr << "FileSink：重新打开日志文件失败：" << AsynGyanis::Platform::FileSystem::utf8FromPath(m_filePath)
                  << "；文件未能打开，后续日志一律不落盘（累计丢弃量可读：skippedLineCount），直到下一次成功 reopen" << '\n';
    }
} // namespace AsynGyanis::Base
