#include "Net/Tracing/Tracer.h"

#include "Base/Exception/LogicException.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 待出口缓冲条数上界的天花板
         * @details 一条记录光是骨架就占 200 字节上下（三段定长标识 + 操作名 + 维度表），再往上调
         *          就不是「攒一批」而是「压一库」：出口失联时这部分内存谁也不会再放掉。
         *          65536 条封顶约合 13 MiB 常驻，够一个正常进程把出口抖动的那几秒垫过去
         */
        constexpr std::size_t kPendingSpanCountCeiling = 65536U;

        /// 采样分桶的总数：比例按这个数折算成阈值，桶号取 trace-id 文本哈希的低 16 位
        constexpr std::uint64_t kSamplingBucketCount = 65536ULL;

        /**
         * @brief 从 trace-id 文本折出一个稳定的采样桶号（0~65535）
         * @details 用 FNV-1a 而不是直接取前 16 位：判据只要「同一个 trace-id 在任何进程里落进同一个桶」，
         *          哈希让这件事与上游怎么填 trace-id 无关（有厂商会把时间戳写进高位）。
         * @param traceIdText 32 位小写十六进制的链路标识文本
         * @return std::uint64_t 桶号
         */
        constexpr std::uint64_t samplingBucketOf(const std::string_view traceIdText) noexcept
        {
            std::uint64_t hash = 14695981039346656037ULL;
            for (const char character: traceIdText)
            {
                hash ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(character));
                hash *= 1099511628211ULL;
            }
            return hash & (kSamplingBucketCount - 1ULL);
        }

        /// @brief 抛出一条配置不成立的中文原因（带字段名与当下取值）
        [[noreturn]] void rejectConfiguration(const std::string &reason)
        {
            throw Base::LogicException("链路编排器无法启动：" + reason);
        }
    } // namespace

    std::shared_ptr<Tracer> Tracer::create(Configuration configuration)
    {
        if (configuration.serviceName.empty())
        {
            rejectConfiguration("服务名为空。出口侧的 service.name 是所有链路检索的入口，留空等于这批节没有出处；"
                                "请在 Tracer::Configuration::serviceName 里填一个服务名");
        }
        // NaN 与 ratio 的任何比较都是假，既过不了「按比例采」也过不了「按比例丢」，只能在入口拒掉
        if (std::isnan(configuration.sampleRatio) || configuration.sampleRatio < 0.0 || configuration.sampleRatio > 1.0)
        {
            rejectConfiguration("采样比例必须是 0.0~1.0 之间的数（当前 " + std::to_string(configuration.sampleRatio) + "）");
        }
        if (configuration.maximumPendingSpanCount == 0U)
        {
            rejectConfiguration("待出口缓冲的条数上界为 0：那不是「不限量」而是「一条都不收」，"
                                "配错了要当场看见，不要退化成静默丢弃");
        }
        if (configuration.exportBatchSpanCount == 0U)
        {
            rejectConfiguration("出口批量为 0：出口线程会一轮一轮地空转。请至少给 1");
        }
        if (configuration.exportInterval <= std::chrono::milliseconds::zero())
        {
            rejectConfiguration("出口时限必须大于 0：只按批量出口的话，低频进程的节会一直躺在缓冲里等到进程退出");
        }

        // 上界钳制：配错的方向是「占更多内存」，把它拉回有界即可，不必让启动失败
        configuration.maximumPendingSpanCount = std::min(configuration.maximumPendingSpanCount, kPendingSpanCountCeiling);
        configuration.exportBatchSpanCount    = std::min(configuration.exportBatchSpanCount, configuration.maximumPendingSpanCount);

        // 这里用不上 make_shared：真构造函数是私有的（只允许 create() 走），make_shared 的分配器
        // 不是友元、够不到它。裸 new 与 shared_ptr 构造在同一个表达式里完成，中途抛出也不会漏所有权
        return std::shared_ptr<Tracer>(new Tracer(std::move(configuration)));
    }

    Tracer::Tracer(Configuration configuration) :
        m_configuration(std::move(configuration)), m_resource{.serviceName = m_configuration.serviceName, .serviceVersion = m_configuration.serviceVersion}
    {
        // 出口线程要读上面全部成员，因此它必须是最后初始化的那一个；jthread 在析构时会自行
        // request_stop + join，但那条路径叫不醒条件变量的等待谓词，~Tracer() 里按 AsyncSink
        // 的同一套规矩在锁内发布停止再叫醒
        m_workerThread = std::jthread([this](const std::stop_token &stopToken) { workerLoop(stopToken); });
        m_stopToken    = m_workerThread.get_stop_token();
    }

    Tracer::~Tracer()
    {
        {
            // 停止标记与等待谓词在同一把锁下发布：锁外通知会落进「等待方已判定谓词为假、尚未入睡」
            // 的窗口被丢弃，出口线程就永远睡在条件变量上，随后的 join() 永久阻塞
            const std::lock_guard lock(m_stateMutex);
            m_workerThread.request_stop();
            // 两类等待者各有各的条件变量，两条都要叫：只叫一条会把另一类留在睡梦里
            m_workCondition.notify_all();
            m_drainedCondition.notify_all();
        }
        // join 之后才关出口：worker 退出前会把缓冲里的残留送完，出口此刻还在被使用
        m_workerThread.join();

        for (const std::shared_ptr<SpanExporter> &exporter: m_exporters)
        {
            // 契约要求 shutdown() 不抛；这里握着的是析构路径，抛上来就是 terminate
            exporter->shutdown();
        }
    }

    void Tracer::addExporter(std::shared_ptr<SpanExporter> exporter)
    {
        if (exporter == nullptr)
        {
            // nullptr 按「没挂」处理：调用方多半是从配置里读出来的出口列表，
            // 这里抛异常会让一个空条目打死整条链路
            return;
        }
        const std::lock_guard lock(m_stateMutex);
        m_exporters.push_back(std::move(exporter));
        // 新出口挂上就叫一次线程：之前攒在缓冲里的节不该等到下一个时限点才有人收
        m_workCondition.notify_one();
    }

    std::size_t Tracer::exporterCount() const noexcept
    {
        const std::lock_guard lock(m_stateMutex);
        return m_exporters.size();
    }

    Span Tracer::startSpan(const std::string_view name, const SpanKind kind, const std::optional<TraceIdentifiers> &parent)
    {
        SpanIdentity identity;
        std::uint8_t flags     = 0U;
        std::uint8_t version   = 0U;
        bool         isSampled = false;

        if (parent.has_value())
        {
            // 上游已判定过的照办（W3C §3.2.1.3：采样位是链路上游的决定，下游只能遵从），
            // 版本号与未定义的标志位一并原样带着走
            version          = parent->version;
            flags            = parent->flags;
            isSampled        = parent->isSampled();
            identity.traceId = parent->traceId;
            // 上游写在 traceparent 里的段标识就是本节的上一节
            identity.parentSpanId = parent->parentId;
            identity.spanId       = generateSpanIdentifier();
        } else
        {
            // 开新链路：先按「不采」生成标识，比例判定要看这个 trace-id 落在哪个桶里
            const TraceIdentifiers fresh = Traceparent::generate(false);
            version                      = fresh.version;
            identity.traceId             = fresh.traceId;
            identity.parentSpanId.fill('\0');
            identity.spanId = generateSpanIdentifier();
            isSampled       = shouldSampleNewTrace(fresh);
            flags           = isSampled ? kSampledTraceFlag : 0U;
        }

        if (!isSampled)
        {
            // 非记录的替身：连名字都不带（名字是唯一一处按长度取堆的字段，而它在这里没有读者）。
            // 标识仍然合法，交给下一跳时采样位是 0，下游于是知道「这条链路没人采」而不是「没有上文」
            return Span{nullptr, identity, flags, version, std::string_view{}, kind};
        }
        return Span{shared_from_this(), identity, flags, version, name, kind};
    }

    void Tracer::flush()
    {
        // 锁对象必须是可变的 unique_lock：wait() 要在等待期间反复解锁/加锁，而且下面几步共用同一份
        std::unique_lock lock(m_stateMutex);
        // 水位取进场那一刻的受理数：后来者的账不是本次要等的账
        const std::uint64_t watermark = m_producedCount;
        // 登记「有人在等」：批量没攒够时这是唯一的叫醒理由，等完就注销
        ++m_flushRequestCount;
        m_workCondition.notify_one();
        m_drainedCondition.wait(lock, [this, watermark] { return m_settledCount >= watermark || m_stopToken.stop_requested(); });
        --m_flushRequestCount;
    }

    std::size_t Tracer::pendingSpanCount() const noexcept
    {
        const std::lock_guard lock(m_stateMutex);
        return m_pending.size();
    }

    std::uint64_t Tracer::exportedSpanCount() const noexcept
    {
        return m_exportedSpanCount.load(std::memory_order_relaxed);
    }

    std::uint64_t Tracer::droppedSpanCount() const noexcept
    {
        return m_droppedSpanCount.load(std::memory_order_relaxed);
    }

    std::uint64_t Tracer::exportFailureCount() const noexcept
    {
        return m_exportFailureCount.load(std::memory_order_relaxed);
    }

    bool Tracer::accept(SpanRecord &&record) noexcept
    {
        try
        {
            const std::lock_guard lock(m_stateMutex);
            if (m_stopToken.stop_requested())
            {
                // 停止窗口里收下的节不会再有人取：留下它们只会让 flush() 等一个永远到不了的水位
                m_droppedSpanCount.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (m_pending.size() >= m_configuration.maximumPendingSpanCount)
            {
                m_droppedSpanCount.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            m_pending.push_back(std::move(record));
            ++m_producedCount;
            if (m_pending.size() >= m_configuration.exportBatchSpanCount)
            {
                // 只叫消费者：这里占用了一个空位而不是腾出一个空位
                m_workCondition.notify_one();
            }
            return true;
        } catch (...)
        {
            // 缓冲扩容失败：这一节没了，计数报出来。为一条链路把异常送回业务路径（再从这里走出去
            // 终止进程）不成比例——本函数由 Span 的析构调用，那条路上没有接异常的人
            m_droppedSpanCount.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    std::vector<SpanRecord> Tracer::takeBatchLocked(const std::size_t maximumCount)
    {
        const std::size_t takeCount = std::min(maximumCount, m_pending.size());
        if (takeCount == 0U)
        {
            return {};
        }
        std::vector<SpanRecord> batch{std::make_move_iterator(m_pending.begin()), std::make_move_iterator(m_pending.begin() + static_cast<std::ptrdiff_t>(takeCount))};
        m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(takeCount));
        return batch;
    }

    std::size_t Tracer::dispatchBatch(std::vector<SpanRecord> &&batch)
    {
        std::vector<std::shared_ptr<SpanExporter>> exporters;
        {
            // 锁内只抄出口表：exportSpans() 会写文件、发请求，握着锁做这些就等于让所有生产者
            // 排在一次慢 IO 后面。抄一份（出口是个位数）换来锁外调用
            const std::lock_guard lock(m_stateMutex);
            exporters = m_exporters;
        }

        const std::size_t batchCount = batch.size();
        for (const std::shared_ptr<SpanExporter> &exporter: exporters)
        {
            bool isAccepted = false;
            try
            {
                isAccepted = exporter->exportSpans(m_resource, batch);
            } catch (...)
            {
                // 出口不许把异常送回这里；送回来也只影响它自己这一路，其余出口照样收
                isAccepted = false;
            }

            if (isAccepted)
            {
                m_exportedSpanCount.fetch_add(batchCount, std::memory_order_relaxed);
            } else
            {
                m_exportFailureCount.fetch_add(1, std::memory_order_relaxed);
                m_droppedSpanCount.fetch_add(batchCount, std::memory_order_relaxed);
            }
        }

        if (exporters.empty())
        {
            // 一个出口都没挂：这批就是白攒了。丢掉并计数，让「配了采样却没配出口」这种写法在
            // droppedSpanCount() 上看得见，而不是把缓冲一直占着
            m_droppedSpanCount.fetch_add(batchCount, std::memory_order_relaxed);
        }

        {
            const std::lock_guard lock(m_stateMutex);
            // 不论送出去还是丢掉，本次受理的账到这里才算清：水位等的是「有人处理过」，不是「处理成功」
            m_settledCount += batchCount;
            if (m_settledCount >= m_producedCount)
            {
                m_drainedCondition.notify_all();
            }
        }
        return batchCount;
    }

    void Tracer::workerLoop(const std::stop_token &stopToken)
    {
        // 下一次按时出口的时刻：每交付一次就重新计，于是「攒够一批」的快节奏不会把时限一路推后
        auto nextDeadline = std::chrono::steady_clock::now() + m_configuration.exportInterval;

        // 可以交付的三种时机：攒够一批、有人在等 flush()、被要求停止。到点另判（见下面那句）
        const auto isDeliveryRequested = [this, &stopToken]
        { return m_pending.size() >= m_configuration.exportBatchSpanCount || m_flushRequestCount > 0U || stopToken.stop_requested(); };

        while (!stopToken.stop_requested())
        {
            std::vector<SpanRecord> batch;
            {
                std::unique_lock lock(m_stateMutex);
                // 三个出口：攒够一批、到一个时限、被要求停止。时限那条是给低频进程留的——
                // 没有它，一两条节会一直躺在缓冲里等到进程退出
                m_workCondition.wait_until(lock, nextDeadline, isDeliveryRequested);

                // 白叫一次不算条件：既没攒够、没人等、也没到点也没要停，就接着睡。标准允许条件变量
                // 在无通知时提前返回，把「取走全部残留」这一步交给这种返回，用例里「一批一次交付」
                // 的计数就会随机器快慢漂移
                const bool isDeadlineReached = std::chrono::steady_clock::now() >= nextDeadline;
                if (!isDeliveryRequested() && !isDeadlineReached)
                {
                    continue;
                }
                batch        = takeBatchLocked(m_configuration.exportBatchSpanCount);
                nextDeadline = std::chrono::steady_clock::now() + m_configuration.exportInterval;
            }
            if (!batch.empty())
            {
                static_cast<void>(dispatchBatch(std::move(batch)));
            }
        }

        // 停止之后仍要把残留送出去：正常退出时最后那一批里有正在处理的请求的链路，
        // 那恰好是最需要看的一段。accept() 在停止后不再收新的，因此这个循环一定结束
        for (;;)
        {
            std::vector<SpanRecord> batch;
            {
                const std::lock_guard lock(m_stateMutex);
                batch = takeBatchLocked(m_configuration.exportBatchSpanCount);
            }
            if (batch.empty())
            {
                break;
            }
            static_cast<void>(dispatchBatch(std::move(batch)));
        }
    }

    bool Tracer::shouldSampleNewTrace(const TraceIdentifiers &identifiers) const noexcept
    {
        // 两端都短路：比例 1.0 时不必哈希，0.0 时一个桶都不给（0.0 的含义是「只采上游点名要的」，
        // 那种请求走的是 parent 那条分支，不经过这里）
        if (m_configuration.sampleRatio >= 1.0)
        {
            return true;
        }
        if (m_configuration.sampleRatio <= 0.0)
        {
            return false;
        }
        const auto threshold = static_cast<std::uint64_t>(m_configuration.sampleRatio * static_cast<double>(kSamplingBucketCount));
        return samplingBucketOf(identifiers.traceIdText()) < threshold;
    }
} // namespace AsynGyanis::Net
