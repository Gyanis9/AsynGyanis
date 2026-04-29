/**
 * @file Cancelable.h
 * @brief Cooperative cancellation mixin using std::stop_token
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_CANCELABLE_H
#define CORE_CANCELABLE_H

#include <stop_token>

namespace Core
{
    class Cancelable
    {
    public:
        Cancelable() = default;

        Cancelable(const Cancelable &)                = delete;
        Cancelable &operator=(const Cancelable &)     = delete;
        Cancelable(Cancelable &&) noexcept            = default;
        Cancelable &operator=(Cancelable &&) noexcept = default;

        bool requestStop() const
        {
            return m_stopSource.request_stop();
        }

        [[nodiscard]] bool isStopRequested() const
        {
            return m_stopSource.stop_requested();
        }

        [[nodiscard]] std::stop_token stopToken() const
        {
            return m_stopSource.get_token();
        }

        [[nodiscard]] std::stop_source &stopSource()
        {
            return m_stopSource;
        }

    private:
        std::stop_source m_stopSource;
    };

}

#endif
