#include "Net/Metrics/OtlpHttpMetricsExporter.h"

#include "Net/Telemetry/OtlpHttpExportSupport.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Tls/TlsPolicy.h"
#include "Net/Metrics/OtlpMetricsJson.h"
#include "Platform/IO/Socket.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 采集端响应的正文上限：OTLP 的回复只可能是一份很小的 partial_success JSON
        constexpr std::size_t kMaximumOtlpResponseBodyBytes = 64U * 1024U;

        /// 那条到采集端的连接空闲多久就收掉：留着一条长期不用的连接不如让对端说了算
        constexpr std::chrono::milliseconds kOutboundIdleTimeout{30000};


        /// 停止察觉切片：叫醒导出协程的唯一办法是让它自己醒来，而 shutdown() 只落一个标志——
        /// 整段睡下去时收尾要等到下一拍。切片只改醒来频次，导出节拍仍由 exportInterval 决定
        constexpr std::chrono::milliseconds kStopCheckSlice{200};

        /// @brief 抛出一条配置不成立的中文原因（带字段名与替代做法）
        [[noreturn]] void rejectConfiguration(const std::string &reason)
        {
            throw Base::InvalidArgumentException("OTLP metrics 出口无法启动：" + reason);
        }


        /// @brief 把采集端地址拆成 ParsedUrl，错误前缀与本出口的配置字段在这里定死
        ParsedUrl makeTarget(const std::string &endpoint)
        {
            return Telemetry::makeOtlpTarget(endpoint, "OTLP metrics 出口无法启动：", "OtlpHttpMetricsExporter::Configuration::endpoint", "/v1/metrics");
        }
    } // namespace

    OtlpHttpMetricsExporter::OtlpHttpMetricsExporter(TraceResource resource, Configuration configuration, SnapshotProvider snapshotProvider) :
        OtlpHttpMetricsExporter(std::move(resource), std::move(configuration), std::move(snapshotProvider), Core::TlsPolicy{})
    {
    }

    OtlpHttpMetricsExporter::OtlpHttpMetricsExporter(TraceResource resource, Configuration configuration, SnapshotProvider snapshotProvider, const Core::TlsPolicy &tlsPolicy) :
        m_configuration(std::move(configuration)), m_resource(std::move(resource)), m_snapshotProvider(std::move(snapshotProvider)), m_target(makeTarget(m_configuration.endpoint))
    {
        if (!m_snapshotProvider)
        {
            rejectConfiguration("快照来源为空：没有取值的地方就一个读数也报不出去。请传入取快照的动作（例如 Core::ProcessMetricsRegistry::samples）");
        }
        if (m_configuration.requestTimeout <= std::chrono::milliseconds::zero() || m_configuration.exportInterval <= std::chrono::milliseconds::zero())
        {
            rejectConfiguration("单次请求时限与导出周期都必须大于 0，否则导出会没有终点、周期会永远空转");
        }
        Telemetry::validateOtlpExtraHeaders(m_configuration.extraHeaders, "OTLP metrics 出口无法启动：");

        HttpOutboundConnectionPool::Config poolConfig;
        poolConfig.idleTimeout              = kOutboundIdleTimeout;
        poolConfig.maximumIdlePerEndpoint   = 1U; ///< 只对一个采集端发，留一条就够
        poolConfig.maximumResponseBodyBytes = kMaximumOtlpResponseBodyBytes;
        // 客户端在构造期就建好：TLS 策略被 OpenSSL 拒绝时要从这里抛出，而不是等第一条发不出去的导出
        m_client = std::make_unique<HttpClient>(m_loop, poolConfig, tlsPolicy);

        // 循环线程最后起：它一起就会读上面全部成员
        m_workerThread = std::jthread([this] { runLoopThread(); });
    }

    OtlpHttpMetricsExporter::~OtlpHttpMetricsExporter()
    {
        // 成员析构顺序在声明的末尾（m_workerThread 最后声明、最先析构），jthread 会在这里 join；
        // 但 join 之前得先让那条循环有理由停下来，否则 run() 永不返回、这里就永远等下去
        shutdown();
    }

    void OtlpHttpMetricsExporter::shutdown() noexcept
    {
        m_isExporting.store(false, std::memory_order_release);
        {
            // 等导出协程退出：最坏是「一次在途请求 + 一个停止察觉切片」。期限到还没退（采集端把请求
            // 拖满整个时限）也照走——帧由 runLoopThread 的栈上 Task 在 run() 返回后收掉，
            // 正挂着的定时等待会随等待器析构自行摘除登记
            std::unique_lock lock(m_pumpExitMutex);
            static_cast<void>(m_pumpExitCondition.wait_for(lock, m_configuration.requestTimeout + kStopCheckSlice * 2, [this] { return m_hasPumpExited; }));
        }
        // EventLoop::stop() 本身线程安全；返回后那条线程会自己把 run() 收尾
        m_loop.stop();
    }

    std::uint64_t OtlpHttpMetricsExporter::deliveredExportCount() const noexcept
    {
        return m_deliveredExportCount.load(std::memory_order_relaxed);
    }

    std::uint64_t OtlpHttpMetricsExporter::failedExportCount() const noexcept
    {
        return m_failedExportCount.load(std::memory_order_relaxed);
    }

    std::uint64_t OtlpHttpMetricsExporter::emptySnapshotCount() const noexcept
    {
        return m_emptySnapshotCount.load(std::memory_order_relaxed);
    }

    std::string OtlpHttpMetricsExporter::lastFailureReason() const
    {
        const std::lock_guard lock(m_reasonMutex);
        return m_lastFailureReason;
    }

    void OtlpHttpMetricsExporter::runLoopThread()
    {
        // 这条线程自己发起出站连接：Windows 上 Winsock 的初始化引用要自己拿一份，
        // 不能靠主线程那一份的存续（POSIX 上这是空操作）
        const Platform::Socket::Initialization network;

        {
            // 导出协程首次恢复必须在循环线程上：在调用线程 resume 会让它就地注册观察者，
            // 与这里的 run() 抢同一份后端
            Core::Task<void> pump = exportOncePerInterval();
            static_cast<void>(pump.handle().resume());
            m_loop.run();
            // 帧必须先于它引用的客户端收掉：帧里握着 HttpClient &client，反过来先 reset 客户端
            // 会让帧析构时碰的等待器指向一块已经释放的对象
        }
        // HttpClient 与它的池都归属这条循环，必须在 run() 返回之后、仍在本线程上收掉；
        // 上面的花括号保证这一步排在帧析构之后
        m_client.reset();
    }

    Core::Task<void> OtlpHttpMetricsExporter::exportOncePerInterval()
    {
        HttpClient &client = *m_client;
        // 周期等待只是本循环的定时队列上一批句柄，构造与析构都不产生系统调用，因此放在协程帧里
        Core::Timer cadence{m_loop};
        const auto  slice = std::min(m_configuration.exportInterval, kStopCheckSlice);
        while (isStillExporting())
        {
            // 等满一个导出周期：按切片醒来、片间复查停止标记（叫醒这条帧的唯一办法是它自己醒来）
            for (auto elapsed = std::chrono::milliseconds{0}; elapsed < m_configuration.exportInterval && isStillExporting(); elapsed += slice)
            {
                co_await cadence.waitFor(std::min(slice, m_configuration.exportInterval - elapsed));
            }
            if (!isStillExporting())
            {
                break;
            }

            const OtlpMetricsBatch snapshot = m_snapshotProvider();
            if (snapshot.isEmpty())
            {
                // 「没有指标可报」不是「通路坏了」：跳过本次并计数；发一份没有指标的正文没有意义
                static_cast<void>(m_emptySnapshotCount.fetch_add(1, std::memory_order_relaxed));
                continue;
            }

            const std::string body = formatOtlpMetricsJson(m_resource, snapshot, std::chrono::system_clock::now());
            if (body.empty())
            {
                static_cast<void>(m_failedExportCount.fetch_add(1, std::memory_order_relaxed));
                {
                    const std::lock_guard reasonLock(m_reasonMutex);
                    m_lastFailureReason = "OTLP metrics 出口：这次的正文没能成形（取值里有进不了 JSON 的字节），本次跳过";
                }
                continue;
            }

            HttpClientRequest request;
            request.method      = "POST";
            request.contentType = kOtlpMetricsJsonContentType;
            request.body        = body;
            request.headers     = m_configuration.extraHeaders;

            const Telemetry::OtlpHttpExportAttempt attempt =
                    co_await Telemetry::sendOtlpHttpRequest(client, m_configuration.endpoint, request, m_configuration.requestTimeout, "OTLP metrics 出口");

            if (attempt.failureReason.empty())
            {
                static_cast<void>(m_deliveredExportCount.fetch_add(1, std::memory_order_relaxed));
            } else
            {
                static_cast<void>(m_failedExportCount.fetch_add(1, std::memory_order_relaxed));
                {
                    const std::lock_guard reasonLock(m_reasonMutex);
                    m_lastFailureReason = attempt.failureReason;
                }
                // 不做额外退避：快照语义下每个周期本来就发最新全量值，下个周期就是天然的重试——
                // 照对端说的等下去只会让「指标明明在变」晚到一个没有意义的时刻（与链路出口的批语义刻意不同）
            }
        }

        {
            const std::lock_guard lock(m_pumpExitMutex);
            m_hasPumpExited = true;
        }
        m_pumpExitCondition.notify_all();
    }

    bool OtlpHttpMetricsExporter::isStillExporting() const noexcept
    {
        return m_isExporting.load(std::memory_order_acquire);
    }
} // namespace AsynGyanis::Net
