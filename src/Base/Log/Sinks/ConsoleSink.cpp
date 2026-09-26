#include "Base/Log/Sinks/ConsoleSink.h"
#include "Base/Log/Formatters/ColorFormatter.h"
#include "Base/Log/Formatters/DefaultFormatter.h"
#include "Base/Log/LogLevel.h"
#include "Platform/IO/Console.h"

#include <iostream>
#include <memory>
#include <mutex>
#include <string>

namespace AsynGyanis::Base
{
    ConsoleSink::ConsoleSink(const bool enableColor) : m_colorEnabled(enableColor)
    {
        AsynGyanis::Platform::Console::ensureUtf8Output();
        // 构造只是「带初值」的一次切换，与运行期切换共用同一条 formatter 选择路径
        applyFormatter();
    }

    void ConsoleSink::write(const LogEvent &event)
    {
        std::lock_guard lock(m_mutex);
        // 整行只做一次 <<：每次流插入都要构造 sentry（锁流缓冲、冲刷被 tie 的流），合并后只有一轮。
        // 版式直接落在留了容量的成员缓冲上——std::format 交回一个新串要取两次堆，
        // 而控制台这条路径每行都要走一遍这里
        m_lineBuffer.clear();
        formatEventInto(m_lineBuffer, event);
        m_lineBuffer.push_back('\n');
        if (event.level >= LogLevel::Warn)
        {
            // std::cerr 恒为 unitbuf：整行写出即落地，无需额外刷新
            std::cerr << m_lineBuffer;
            reportStreamFailureOnceLocked(std::cerr, m_hasReportedStderrFailure, "stderr");
        } else
        {
            // 每条刷新：std::cout 重定向到文件或管道时是全缓冲，不刷就 tail 不到实时内容，
            // 异常退出还会丢掉尾部（实测 +1 µs/行，Release /O2），换来 write() 返回即已落地
            std::cout << m_lineBuffer;
            std::cout.flush();
            reportStreamFailureOnceLocked(std::cout, m_hasReportedStdoutFailure, "stdout");
        }
    }

    void ConsoleSink::flush()
    {
        std::lock_guard lock(m_mutex);
        std::cout.flush();
        std::cerr.flush();
        // 刷完要回头看流状态：目标管道另一头已经退出时，插入与刷新都只把 badbit 立起来而不抛，
        // 此后每一行都是空操作。「write() 返回即已落地」这句承诺没有这一步就只是断言
        reportStreamFailureOnceLocked(std::cout, m_hasReportedStdoutFailure, "stdout");
        reportStreamFailureOnceLocked(std::cerr, m_hasReportedStderrFailure, "stderr");
    }

    void ConsoleSink::reportStreamFailureOnceLocked(std::ostream &stream, bool &hasReported, const char *const streamLabel)
    {
        if (stream.good())
        {
            // 成功写过一次就重新武装：下一次故障还要出声
            hasReported = false;
            return;
        }
        if (hasReported)
        {
            return;
        }
        hasReported = true;
        // 诊断交给另一条流：本 Sink 就是日志出口，拿根日志器报自己等于让 write() 递归回来
        std::ostream &escape = (&stream == &std::cout) ? std::cerr : std::cout;
        escape << "ConsoleSink：" << streamLabel << " 写出失败（目标可能已被关闭，或重定向到了已退出的读取端）；流已失效，该流上的后续日志不会再输出" << '\n';
        escape.flush();
    }

    void ConsoleSink::setColorEnabled(const bool enabled)
    {
        std::lock_guard lock(m_mutex);
        m_colorEnabled = enabled;
        applyFormatter();
    }

    void ConsoleSink::applyFormatter()
    {
        // 终端不支持 ANSI 序列时即便请求了彩色也退回纯文本，避免输出乱码
        if (m_colorEnabled && AsynGyanis::Platform::Console::supportsAnsiEscapeCodes())
        {
            setFormatter(std::make_unique<ColorFormatter>());
        } else
        {
            setFormatter(std::make_unique<DefaultFormatter>());
        }
    }
} // namespace AsynGyanis::Base
