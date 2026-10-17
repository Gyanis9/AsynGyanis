#include "Base/Log/LogContext.h"

namespace AsynGyanis::Base
{
    namespace
    {
        /// 当前线程的日志上下文。放 .cpp 而不是头里 inline：共享库形态下头内联的 thread_local
        /// 每个模块各一份，「进程内唯一」就不成立（见 MSVC 导出形状那组坑）
        thread_local LogContextPtr t_currentLogContext;
    } // namespace

    const LogContextPtr &currentLogContext() noexcept
    {
        return t_currentLogContext;
    }

    void publishLogContext(LogContextPtr context) noexcept
    {
        t_currentLogContext = std::move(context);
    }

    LogContextScope::LogContextScope(const std::initializer_list<std::pair<std::string_view, std::string_view>> fields) : m_previous(currentLogContext())
    {
        std::map<std::string, std::string> merged;
        if (m_previous != nullptr)
        {
            merged = m_previous->fields();
        }

        for (const auto &[key, value]: fields)
        {
            if (value.empty())
            {
                static_cast<void>(merged.erase(std::string(key)));
                continue;
            }
            merged.insert_or_assign(std::string(key), std::string(value));
        }

        m_current = std::make_shared<const LogContext>(std::move(merged));
        publishLogContext(m_current);
    }

    LogContextScope::~LogContextScope()
    {
        publishLogContext(m_previous);
    }
} // namespace AsynGyanis::Base
