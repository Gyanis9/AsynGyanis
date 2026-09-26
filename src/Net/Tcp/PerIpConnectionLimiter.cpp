#include "Net/Tcp/PerIpConnectionLimiter.h"

#include <cstddef>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// IPv4 映射地址的文本前缀：glibc 与 Windows 的 inet_ntop 都按 `::ffff:a.b.c.d` 打印
        constexpr std::string_view mappedIpv4Prefix = "::ffff:";

        /**
         * @brief 只看 ASCII 字母的大小写归一（不动其余字节，也不引入 locale）
         * @param value 待比较的字节
         * @return char 小写形式
         */
        constexpr char toLowerAscii(const char value) noexcept
        {
            return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
        }

        /**
         * @brief 判断是不是「四段 1~3 位十进制、每段不超过 255」的点分 IPv4 文本
         * @param value 候选文本
         * @return true 是
         */
        bool isDottedDecimalIpv4(const std::string_view value) noexcept
        {
            std::size_t groupCount = 0;
            std::size_t digitCount = 0;
            unsigned int groupValue = 0;
            for (std::size_t index = 0; index <= value.size(); ++index)
            {
                const char current = index == value.size() ? '.' : value[index];
                if (current >= '0' && current <= '9')
                {
                    // 段内超三位即不再是合法点分文本（也挡掉了前导零堆出的长串）
                    if (++digitCount > 3)
                    {
                        return false;
                    }
                    groupValue = groupValue * 10U + static_cast<unsigned int>(current - '0');
                    continue;
                }
                if (current != '.' || digitCount == 0 || groupValue > 255U)
                {
                    return false;
                }
                ++groupCount;
                digitCount = 0;
                groupValue = 0;
            }
            return groupCount == 4;
        }

        /**
         * @brief 把来源键折成规范形式：`::ffff:a.b.c.d` 去掉前缀，只留点分十进制本体
         * @details 双栈监听器（本框架显式关掉 IPV6_V6ONLY）上的 IPv4 客户端，对端地址族是 AF_INET6、
         *          文本带 `::ffff:` 前缀（Windows 与 glibc 实测同此形式）。同一来源若还从纯 IPv4
         *          监听器进来，键里没有这个前缀——两种写法各占一格，「单个来源」的上限实际翻倍。
         *          前缀之外的部分保持原样：IPv6 文本本身已是规范形式，改写它只会掩盖真正的差异。
         * @param ipKey 调用方给出的来源键
         * @return std::string 规范化后的来源键
         */
        std::string normalizeIpKey(const std::string &ipKey)
        {
            if (ipKey.size() <= mappedIpv4Prefix.size())
            {
                return ipKey;
            }
            for (std::size_t index = 0; index < mappedIpv4Prefix.size(); ++index)
            {
                if (toLowerAscii(ipKey[index]) != mappedIpv4Prefix[index])
                {
                    return ipKey;
                }
            }
            const std::string_view tail(ipKey.data() + mappedIpv4Prefix.size(), ipKey.size() - mappedIpv4Prefix.size());
            return isDottedDecimalIpv4(tail) ? std::string{tail} : ipKey;
        }
    } // namespace

    PerIpConnectionLimiter::PerIpConnectionLimiter(const std::size_t maximumConnectionsPerIp) :
        m_state(std::make_shared<State>()), m_maximumConnectionsPerIp(maximumConnectionsPerIp)
    {
    }

    PerIpConnectionLimiter::Lease::Lease(std::shared_ptr<State> state, std::string ipKey) :
        m_state(std::move(state)), m_ipKey(std::move(ipKey))
    {
    }

    PerIpConnectionLimiter::Lease::Lease(Lease &&other) noexcept :
        m_state(std::move(other.m_state)), m_ipKey(std::move(other.m_ipKey))
    {
        // 把被移走的一方置空：否则它的析构会把同一个名额再还一次，计数越还越少、上限被悄悄放大
        other.m_state.reset();
    }

    PerIpConnectionLimiter::Lease &PerIpConnectionLimiter::Lease::operator=(Lease &&other) noexcept
    {
        if (this != &other)
        {
            // 先归还自己手里的名额，再接管对方的；直接覆盖会让本对象原有的名额漏还
            releaseIfTracking();
            m_state = std::move(other.m_state);
            m_ipKey = std::move(other.m_ipKey);
            other.m_state.reset();
        }
        return *this;
    }

    PerIpConnectionLimiter::Lease::~Lease()
    {
        releaseIfTracking();
    }

    bool PerIpConnectionLimiter::Lease::isTracking() const noexcept
    {
        return m_state != nullptr;
    }

    void PerIpConnectionLimiter::Lease::releaseIfTracking() noexcept
    {
        if (m_state != nullptr)
        {
            PerIpConnectionLimiter::release(*m_state, m_ipKey);
            m_state.reset();
        }
    }

    std::optional<PerIpConnectionLimiter::Lease> PerIpConnectionLimiter::tryAcquire(std::string ipKey)
    {
        // 限额为 0 表示关闭该项保护：既不记账也不拒绝，交出一个空壳凭据，
        // 调用方因此不必在「有保护」与「没保护」两条路径上分别写要不要还名额
        if (m_maximumConnectionsPerIp == 0)
        {
            return Lease{};
        }

        // 记账与归还都用规范化后的键，因此凭据里保管的也是它：还的时候必须找得到同一格
        ipKey = normalizeIpKey(ipKey);

        std::lock_guard<std::mutex> guard(m_state->mutex);

        // 用 find 而不是 operator[]：后者会为每个来过的来源留下一个 0 计数条目，
        // 被拒的连接也会把来源地址写进表里，长期运行下这张表只增不减
        const auto iterator = m_state->activeCounts.find(ipKey);
        if (iterator == m_state->activeCounts.end())
        {
            // 首次见到这个来源：插入而不是自增（ipKey 还要交给凭据保管，故传拷贝）
            m_state->activeCounts.emplace(ipKey, 1);
        }
        else
        {
            if (iterator->second >= m_maximumConnectionsPerIp)
            {
                // 拒绝要留数：这道闸门挡住人时连接根本不存在，除了这一笔计数没有任何现场可查
                m_state->rejectedConnectionCount.fetch_add(1, std::memory_order_relaxed);
                return std::nullopt;
            }
            ++iterator->second;
        }

        return Lease{m_state, std::move(ipKey)};
    }

    std::size_t PerIpConnectionLimiter::activeCountFor(const std::string &ipKey) const
    {
        // 与 tryAcquire 同一套规范化：查询侧不折键就会看着像「这个来源一条都没占」，
        // 而记账其实发生在去掉前缀的那一格上
        const std::string normalizedKey = normalizeIpKey(ipKey);
        std::lock_guard<std::mutex> guard(m_state->mutex);
        const auto iterator = m_state->activeCounts.find(normalizedKey);
        return iterator == m_state->activeCounts.end() ? 0 : iterator->second;
    }

    std::uint64_t PerIpConnectionLimiter::rejectedConnectionCount() const noexcept
    {
        // 不取计数表的锁：这是个自增计数，读它的人要的是「这段时间挡了多少」，不是与在册表的一致性
        return m_state->rejectedConnectionCount.load(std::memory_order_relaxed);
    }

    void PerIpConnectionLimiter::release(State &state, const std::string &ipKey)
    {
        std::lock_guard<std::mutex> guard(state.mutex);

        const auto iterator = state.activeCounts.find(ipKey);
        if (iterator == state.activeCounts.end())
        {
            // 表里没有这个来源：只可能是重复归还。照旧扣减会让计数越还越少，把上限悄悄放大，
            // 所以宁可什么都不做——凭据的移动语义已经保证正常路径下不会走到这里
            return;
        }

        if (--iterator->second == 0)
        {
            // 计数归零即删条目：否则这张表会按「历史见过的来源数」只增不减
            state.activeCounts.erase(iterator);
        }
    }

} // namespace AsynGyanis::Net
