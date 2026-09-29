#include "Base/Log/LogEvent.h"

namespace AsynGyanis::Base
{
    const std::shared_ptr<const std::string> &threadIdString()
    {
        // 每个线程只构造一次：日志热路径上取值只是复制一个 shared_ptr，不重复格式化、不额外分配。
        // 快照在首次取用时构造，之后不再改写，因此异步 Sink 把它投递到别的线程也只是延长寿命
        thread_local const std::shared_ptr<const std::string> kcachedThreadId = []
        {
            std::array<char, 24> buffer{};
            const auto [out, errorCode] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), std::hash<std::thread::id>{}(std::this_thread::get_id()));
            return std::make_shared<const std::string>(buffer.data(), static_cast<std::string::size_type>(out - buffer.data()));
        }();
        return kcachedThreadId;
    }

} // namespace AsynGyanis::Base
