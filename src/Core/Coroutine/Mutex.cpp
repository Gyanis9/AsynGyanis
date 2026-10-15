#include "Core/Coroutine/Mutex.h"

namespace AsynGyanis::Core
{
    Mutex::Mutex(EventLoop &loop) noexcept : m_gate(loop, 1)
    {
    }

    Mutex::~Mutex() = default;

    Mutex::Awaiter Mutex::lock()
    {
        return m_gate.acquire(1);
    }

    std::optional<Mutex::Guard> Mutex::tryLock()
    {
        return m_gate.tryAcquire(1);
    }
} // namespace AsynGyanis::Core
