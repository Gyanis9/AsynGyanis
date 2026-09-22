#include "Core/Socket/VectoredSendCursor.h"

namespace AsynGyanis::Core
{
    namespace detail
    {
        VectoredSendCursor::VectoredSendCursor(const Platform::Socket::WriteBuffer *const buffers, const std::size_t bufferCount) noexcept :
            m_buffers(buffers), m_bufferCount(bufferCount)
        {
            for (std::size_t index = 0; index < m_bufferCount; ++index)
            {
                m_totalLength += m_buffers[index].length;
            }
        }

        std::size_t VectoredSendCursor::snapshotPending(Platform::Socket::WriteBuffer *const outBuffers, const std::size_t capacity) const noexcept
        {
            std::size_t pendingCount = 0;

            // 首段按游标偏移切掉已发部分：偏移已经等于段长（整段发完了）或段长为 0 时整段跳过，
            // 否则从「段起点 + 偏移」接着发。首段同样受 capacity 约束——它是唯一一个写在
            // 「循环条件里的那个判定」之外的写入，容量为 0 时不判就会写进调用方长度为零的数组
            if (pendingCount < capacity && m_pendingIndex < m_bufferCount)
            {
                const Platform::Socket::WriteBuffer &pending = m_buffers[m_pendingIndex];
                if (m_offsetInPending < pending.length)
                {
                    outBuffers[pendingCount].data =
                            static_cast<const char *>(pending.data) + m_offsetInPending;
                    outBuffers[pendingCount].length = pending.length - m_offsetInPending;
                    ++pendingCount;
                }
            }

            // 其余段整段交出，但空段不占提交位：占位会把真正待发的段挤出本次提交，
            // 多付一次系统调用（快照的契约就是「只交出真正待发的字节」）
            for (std::size_t index = m_pendingIndex + 1; index < m_bufferCount && pendingCount < capacity; ++index)
            {
                if (m_buffers[index].length == 0)
                {
                    continue;
                }
                outBuffers[pendingCount] = m_buffers[index];
                ++pendingCount;
            }
            return pendingCount;
        }

        void VectoredSendCursor::advance(const std::size_t byteCount) noexcept
        {
            if (byteCount == 0 || isFinished())
            {
                return;
            }

            m_sentLength += byteCount;

            // 一次写可能跨过若干整段，也可能停在其中某一段中间：按剩余量逐段消账
            std::size_t remainingAdvance = byteCount;
            while (remainingAdvance > 0 && m_pendingIndex < m_bufferCount)
            {
                const std::size_t remainingInPending = m_buffers[m_pendingIndex].length - m_offsetInPending;
                if (remainingAdvance < remainingInPending)
                {
                    m_offsetInPending += remainingAdvance;
                    return;
                }
                remainingAdvance -= remainingInPending;
                ++m_pendingIndex;
                m_offsetInPending = 0;
            }

            skipExhaustedSegments();
        }

        bool VectoredSendCursor::isFinished() const noexcept
        {
            return m_sentLength >= m_totalLength;
        }

        std::size_t VectoredSendCursor::totalLength() const noexcept
        {
            return m_totalLength;
        }

        std::size_t VectoredSendCursor::sentLength() const noexcept
        {
            return m_sentLength;
        }

        void VectoredSendCursor::skipExhaustedSegments() noexcept
        {
            // 零长度段与「刚好发完的段」都要跳过，否则下一次快照会先交出一个空段，
            // 白白占掉一次提交的段位
            while (m_pendingIndex < m_bufferCount && m_offsetInPending >= m_buffers[m_pendingIndex].length)
            {
                ++m_pendingIndex;
                m_offsetInPending = 0;
            }
        }
    } // namespace detail
} // namespace AsynGyanis::Core
