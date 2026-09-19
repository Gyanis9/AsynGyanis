#include "Net/Quic/QuicReassemblyBuffer.h"

#include <algorithm>
#include <utility>

namespace AsynGyanis::Net
{
    void QuicReassemblyBuffer::insert(const std::uint64_t offset, const std::span<const std::uint8_t> bytes)
    {
        if (bytes.empty() || offset + bytes.size() <= m_deliveredOffset)
        {
            // 整段都在已交付的前缀里：重发而已，没有新字节可补
            return;
        }
        std::uint64_t begin = offset;
        std::span<const std::uint8_t> pendingBytes = bytes;
        if (begin < m_deliveredOffset)
        {
            // §7.5：交付过的字节不再重复交付，只留没见过的尾巴
            const std::size_t alreadyDeliveredByteCount = static_cast<std::size_t>(m_deliveredOffset - begin);
            begin = m_deliveredOffset;
            pendingBytes = pendingBytes.subspan(alreadyDeliveredByteCount);
        }
        std::uint64_t end = begin + pendingBytes.size();
        std::vector<std::uint8_t> data(pendingBytes.begin(), pendingBytes.end());

        // 合并窗口从「可能压住本段左边界的那一格」开始：新段的起点落在缓存段中间时，
        // 只按起点建索引会让那一格永远排不干
        auto cursor = m_fragments.upper_bound(begin);
        if (cursor != m_fragments.begin())
        {
            const auto previous = std::prev(cursor);
            if (previous->first + previous->second.size() >= begin)
            {
                cursor = previous;
            }
        }
        while (cursor != m_fragments.end() && cursor->first <= end)
        {
            const std::uint64_t fragmentBegin = cursor->first;
            const std::uint64_t fragmentEnd = fragmentBegin + cursor->second.size();
            const std::uint64_t mergedBegin = std::min(begin, fragmentBegin);
            const std::uint64_t mergedEnd = std::max(end, fragmentEnd);
            // 两段各按自己的起点抄进同一条新区间，本段的字节后抄，因此重叠处以本次交入的为准。
            // 这样写是为了避开「谁在左谁在右、尾巴还剩多少」这类偏移算术：只有两处减法会出错的可能
            std::vector<std::uint8_t> merged(static_cast<std::size_t>(mergedEnd - mergedBegin), 0U);
            std::ranges::copy(cursor->second, merged.begin() + static_cast<std::ptrdiff_t>(fragmentBegin - mergedBegin));
            std::ranges::copy(data, merged.begin() + static_cast<std::ptrdiff_t>(begin - mergedBegin));
            m_bufferedByteCount -= cursor->second.size();
            begin = mergedBegin;
            end = mergedEnd;
            data = std::move(merged);
            cursor = m_fragments.erase(cursor);
        }
        m_bufferedByteCount += data.size();
        m_fragments[begin] = std::move(data);
    }

    std::size_t QuicReassemblyBuffer::drain(std::vector<std::uint8_t> &out)
    {
        std::size_t drainedByteCount = 0;
        // 覆盖区互不相接，因此「恰好从交付点开始」的那一格就是唯一候选
        for (auto fragment = m_fragments.find(m_deliveredOffset); fragment != m_fragments.end();
             fragment = m_fragments.find(m_deliveredOffset))
        {
            out.insert(out.end(), fragment->second.begin(), fragment->second.end());
            m_deliveredOffset += fragment->second.size();
            m_bufferedByteCount -= fragment->second.size();
            drainedByteCount += fragment->second.size();
            m_fragments.erase(fragment);
        }
        return drainedByteCount;
    }

    std::size_t QuicReassemblyBuffer::bufferedByteCount() const noexcept
    {
        return m_bufferedByteCount;
    }

    std::uint64_t QuicReassemblyBuffer::deliveredOffset() const noexcept
    {
        return m_deliveredOffset;
    }
} // namespace AsynGyanis::Net
