#include "Core/EventLoop/Timer.h"
#include "Core/EventLoop/EventLoop.h"

namespace AsynGyanis::Core
{
    Timer::Timer(EventLoop &loop) noexcept : m_queue(loop.timerQueue())
    {
    }

    Timer::~Timer() = default;

    Timer::Awaiter Timer::waitFor(const std::chrono::milliseconds duration)
    {
        return m_queue.waitFor(duration);
    }

} // namespace AsynGyanis::Core
