#include "QuicReceivedPacketNumbers.h"

#include <iterator>

namespace AsynGyanis::Net
{
    bool QuicReceivedPacketNumbers::insert(const std::uint64_t packetNumber)
    {
        // 候选只有两段：起点大于本号的第一个区间，和它左边那一个。其余区间与本号不相邻也不包含它
        const auto next = m_ranges.upper_bound(packetNumber);
        if (next != m_ranges.begin())
        {
            const auto previous = std::prev(next);
            if (packetNumber <= previous->second)
            {
                // 落在已有区间之内：这就是重复包号，调用方整包丢弃（§13.1 不许二次交付）
                return false;
            }
            if (packetNumber == previous->second + 1U)
            {
                // 正好接在左邻的尾巴上；若右邻的开头也接得上，这一格就把两段缝成一段
                ++m_trackedPacketNumberCount;
                previous->second = packetNumber;
                if (next != m_ranges.end() && next->first == packetNumber + 1U)
                {
                    previous->second = next->second;
                    m_ranges.erase(next);
                }
                return true;
            }
        }
        if (next != m_ranges.end() && next->first == packetNumber + 1U)
        {
            // 接在右邻开头之前：起点前移一格。键不能直接改，用 extract 摘下节点改完再装回去——
            // 自始至终是同一块内存，因此这条路一次分配都不产生
            ++m_trackedPacketNumberCount;
            auto movedRange = m_ranges.extract(next);
            movedRange.key() = packetNumber;
            m_ranges.insert(std::move(movedRange));
            return true;
        }

        m_trackedPacketNumberCount += 1U;
        m_ranges.emplace(packetNumber, packetNumber);
        return true;
    }

    void QuicReceivedPacketNumbers::dropOldestUntil(const std::size_t maximumTracked) noexcept
    {
        while (!m_ranges.empty() && m_trackedPacketNumberCount > maximumTracked)
        {
            const auto oldest = m_ranges.begin();
            const std::size_t oldestSpan = static_cast<std::size_t>(oldest->second - oldest->first + 1U);
            const std::size_t excess = m_trackedPacketNumberCount - maximumTracked;
            if (oldestSpan <= excess)
            {
                // 整段都在额度之外：连着这段一起丢，留下一段空起点没有意义
                m_trackedPacketNumberCount -= oldestSpan;
                m_ranges.erase(oldest);
                continue;
            }
            // 超额比这段短：只截它的开头，留下的仍然是连续区间，也才留得住最新的包号。
            // 摘下节点再取插入位置：截完的键比当时最前的那个还小，所以它必回到 begin()——
            // 按 begin 提示插回去不重新找位置（提示必须在 extract 之后取，extract 会作废之前的迭代器）
            auto trimmed = m_ranges.extract(m_ranges.begin());
            trimmed.key() += excess;
            m_ranges.insert(m_ranges.begin(), std::move(trimmed));
            m_trackedPacketNumberCount -= excess;
        }
    }
} // namespace AsynGyanis::Net
