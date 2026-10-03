#include "Core/Metrics/ProcessMetricsRegistry.h"

#include <algorithm>
#include <format>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace AsynGyanis::Core
{
    namespace
    {
        /// 一条登记的内部形状；把手交出的是 (name, sequence) 这对坐标
        struct Entry
        {
            std::string           name;
            std::string           help;
            ProcessMetricKind     kind{ProcessMetricKind::Gauge};
            ProcessMetricMerge    merge{ProcessMetricMerge::Sum};
            ProcessMetricProvider provider{};
            std::uint64_t         sequence{0};
        };

        /**
         * @brief 注册表的内部状态
         * @details 刻意用**故意不析构的单例**（`new` 出来不还）：把手可能持有在静态存储期的对象里，
         *          那些对象析构时若注册表已经销毁，注销就会打在死对象上。销毁顺序这类问题不值得让
         *          运维在进程退出时撞一次，泄漏的那点内存在进程退出时由系统回收
         */
        struct RegistryState
        {
            std::shared_mutex  mutex;   ///< 读：抓取时遍历并调 provider；写：登记与注销
            std::vector<Entry> entries; ///< 按登记先后排，导出顺序因此稳定可比
            std::uint64_t      nextSequence{1};
        };

        RegistryState &state()
        {
            static RegistryState *instance = new RegistryState();
            return *instance;
        }

    } // namespace

    [[nodiscard]] bool isLegalPrometheusMetricName(const std::string_view name) noexcept
    {
        // 合法形状：Prometheus 认 `[a-zA-Z_:][a-zA-Z0-9_:]*`
        const auto isAllowed = [](const char character)
        {
            return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == ':' ||
                   character == '_';
        };
        const auto isAllowedLead = [](const char character)
        { return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || character == ':' || character == '_'; };

        if (name.empty() || !isAllowedLead(name.front()))
        {
            return false;
        }
        return std::all_of(name.begin() + 1, name.end(), isAllowed);
    }

    ProcessMetricHandle::~ProcessMetricHandle()
    {
        if (!m_registered)
        {
            return;
        }

        RegistryState   &registry = state();
        std::unique_lock guard{registry.mutex};
        std::erase_if(registry.entries, [&](const Entry &entry) { return entry.name == m_name && entry.sequence == m_sequence; });
    }

    ProcessMetricHandle::ProcessMetricHandle(ProcessMetricHandle &&other) noexcept : m_name(std::move(other.m_name)), m_sequence(other.m_sequence), m_registered(other.m_registered)
    {
        other.m_sequence   = 0;
        other.m_registered = false;
    }

    ProcessMetricHandle &ProcessMetricHandle::operator=(ProcessMetricHandle &&other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }

        // 先把手上原来那条注销掉，再接过来者的坐标：反过来写会让赋值变成「丢一条读数」
        if (m_registered)
        {
            RegistryState   &registry = state();
            std::unique_lock guard{registry.mutex};
            std::erase_if(registry.entries, [&](const Entry &entry) { return entry.name == m_name && entry.sequence == m_sequence; });
        }

        m_name             = std::move(other.m_name);
        m_sequence         = other.m_sequence;
        m_registered       = other.m_registered;
        other.m_sequence   = 0;
        other.m_registered = false;
        return *this;
    }

    ProcessMetricHandle ProcessMetricsRegistry::registerMetric(std::string name, std::string help, const ProcessMetricKind kind, const ProcessMetricMerge merge,
                                                               ProcessMetricProvider provider)
    {
        if (!isLegalPrometheusMetricName(name))
        {
            throw std::invalid_argument(std::format("进程级指标的名字不合法：「{}」。合法形状是 [a-zA-Z_:][a-zA-Z0-9_:]*，"
                                                    "建议 asyn_<面>_<指标>_[total|seconds|bytes]，计数类以 _total 结尾",
                                                    name));
        }
        if (!provider)
        {
            throw std::invalid_argument(std::format("进程级指标 {} 的取值回调是空的：登记了名字却没有读数可取，"
                                                    "导出里会留下一条恒为 0 的假读数",
                                                    name));
        }

        // 说明会原样进 `# HELP` 一行，带换行就会把导出格式切开（采集侧会把后半句当成一条无效行丢掉）。
        // 这是登记时的手误而不是运行期状态，归一化处理比拒掉更合适
        std::replace(help.begin(), help.end(), '\n', ' ');
        std::replace(help.begin(), help.end(), '\r', ' ');

        RegistryState   &registry = state();
        std::unique_lock guard{registry.mutex};

        // 同名登记支持（多台同类对象各一份），但类型/说明/并法必须一致：不一致就是两处代码在抢一个名字
        for (const Entry &entry: registry.entries)
        {
            if (entry.name != name)
            {
                continue;
            }
            if (entry.kind != kind || entry.merge != merge || entry.help != help)
            {
                throw std::invalid_argument(std::format("进程级指标 {} 已被登记，且这次登记的类型、说明或并法与先登的那份不一致。"
                                                        "同名会把两条读数并成一个数，抢名字等于悄悄把两个不相干的东西加成一条",
                                                        name));
            }
            break;
        }

        const std::uint64_t sequence = registry.nextSequence++;
        registry.entries.push_back(Entry{std::move(name), std::move(help), kind, merge, std::move(provider), sequence});
        return ProcessMetricHandle(registry.entries.back().name, sequence);
    }

    namespace
    {
        /**
         * @brief `MinNonZero` 的并法：0 表示「这一格读不出」，不参与求早
         * @param current 已合并出来的值
         * @param incoming 后一个实例交回的值
         * @return std::uint64_t 两者里更早的那个数；两边都是 0 时才是 0
         */
        std::uint64_t mergeIgnoringZero(const std::uint64_t current, const std::uint64_t incoming)
        {
            if (current == 0U)
            {
                return incoming;
            }
            if (incoming == 0U)
            {
                return current;
            }
            return std::min(current, incoming);
        }
    } // namespace

    std::vector<ProcessMetricSample> ProcessMetricsRegistry::samples()
    {
        RegistryState   &registry = state();
        std::shared_lock guard{registry.mutex};

        std::vector<ProcessMetricSample> samples;
        samples.reserve(registry.entries.size());
        std::unordered_map<std::string, std::size_t> indexOfName;

        for (const Entry &entry: registry.entries)
        {
            const std::uint64_t value = entry.provider();

            if (const auto position = indexOfName.find(entry.name); position != indexOfName.end())
            {
                ProcessMetricSample &existing = samples[position->second];
                existing.value = entry.merge == ProcessMetricMerge::Sum
                                         ? existing.value + value
                                         : (entry.merge == ProcessMetricMerge::MinNonZero ? mergeIgnoringZero(existing.value, value) : std::min(existing.value, value));
                continue;
            }

            indexOfName.emplace(entry.name, samples.size());
            samples.push_back(ProcessMetricSample{entry.name, entry.help, entry.kind, value});
        }
        return samples;
    }

    std::size_t ProcessMetricsRegistry::nameCount()
    {
        RegistryState   &registry = state();
        std::shared_lock guard{registry.mutex};

        std::size_t count = 0;
        for (std::size_t index = 0; index < registry.entries.size(); ++index)
        {
            const bool isFirstWithThisName = std::find_if(registry.entries.begin(), registry.entries.begin() + index,
                                                          [&](const Entry &entry) { return entry.name == registry.entries[index].name; }) == registry.entries.begin() + index;
            count += isFirstWithThisName ? 1U : 0U;
        }
        return count;
    }
} // namespace AsynGyanis::Core
