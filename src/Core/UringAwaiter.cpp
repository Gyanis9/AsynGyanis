#include "UringAwaiter.h"

namespace Core
{
    thread_local std::unordered_map<void *, UringAwaiter *> UringAwaiter::s_pending;
}
