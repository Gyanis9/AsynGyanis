#include "Core/Process/SignalObserverInstall.h"

#include "Base/Log/LogMacros.h"

#include <exception>
#include <string>
#include <utility>

namespace AsynGyanis::Core
{
    std::unique_ptr<Platform::SignalWatcher> installSignalObserver(Platform::SignalSet signals, Platform::SignalWatcher::Handler handler, const std::string_view subject,
                                                                   const std::string_view fallback, const std::string_view unavailableReason) noexcept
    {
        // 构造失败与平台不可用都只记一条日志并放手：信号接管是尽力而为，两条观察者都另有程序内触发入口
        try
        {
            auto watcher = std::make_unique<Platform::SignalWatcher>(signals, std::move(handler),
                                                                     [name = std::string(subject)](const std::string_view message) { LOG_ERROR_FMT("{}: {}", name, message); });
            if (!watcher->isInstalled())
            {
                LOG_ERROR_FMT("{}: 信号接管不会装上（{}）；{}", subject, unavailableReason, fallback);
                return nullptr;
            }
            return watcher;
        } catch (const std::exception &failure)
        {
            LOG_ERROR_FMT("{}: 信号接管不会装上（安装信号观察者失败：{}）；{}", subject, failure.what(), fallback);
        } catch (...)
        {
            LOG_ERROR_FMT("{}: 信号接管不会装上（安装信号观察者失败：非标准异常）；{}", subject, fallback);
        }
        return nullptr;
    }
} // namespace AsynGyanis::Core
