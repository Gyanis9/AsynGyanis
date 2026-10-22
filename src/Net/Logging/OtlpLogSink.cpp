#include "Net/Logging/OtlpLogSink.h"

#include "Net/Telemetry/OtlpHttpExportSupport.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Log/LogContext.h"
#include "Core/EventLoop/Timer.h"
#include "Net/Tracing/OtlpHttpSpanExporter.h"
#include "Platform/Platform.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        using Telemetry::hasUnsafeHeaderByte;
        using Telemetry::isReservedHeaderName;

        /// 一次 POST 最多收这么多响应字节：OTLP 的 2xx 正文是空的，留一点余量就够
        constexpr std::size_t kMaximumOtlpResponseBodyBytes = 64U * 1024U;

        /// 出站连接的空闲上限：只对一个采集端发，留一条就够
        constexpr std::chrono::milliseconds kOutboundIdleTimeout{30000};

        /// 退出等待的切片：停止标记最多这么久被看到一次
        constexpr std::chrono::milliseconds kStopPollInterval{100};


        /// @brief 抛出一条配置不成立的中文原因
        [[noreturn]] void rejectConfiguration(const std::string &reason)
        {
            throw Base::InvalidArgumentException("OTLP 日志出口无法启动：" + reason);
        }


        /// @brief 把采集端地址拆成 ParsedUrl，错误前缀与本出口的配置字段在这里定死
        ParsedUrl makeTarget(const std::string &endpoint)
        {
            return Telemetry::makeOtlpTarget(endpoint, "OTLP 日志出口无法启动：", "log_export.otlp.endpoint", "/v1/logs");
        }
    } // namespace

    OtlpLogSink::OtlpLogSink(TraceResource resource, Configuration configuration) : m_configuration(std::move(configuration)), m_resource(std::move(resource))
    {
        // 构造期把地址与头部都验一遍：留到第一条发不出去才报，现场只剩「日志平台没数据」
        static_cast<void>(makeTarget(m_configuration.endpoint));
        if (m_configuration.exportInterval <= std::chrono::milliseconds::zero() || m_configuration.requestTimeout <= std::chrono::milliseconds::zero() ||
            m_configuration.shutdownTimeout <= std::chrono::milliseconds::zero())
        {
            rejectConfiguration("攒批周期、单次请求时限与收尾期限都必须大于 0，否则发送会没有终点、收尾会永远等下去");
        }
        if (m_configuration.maximumPendingRecordCount == 0U || m_configuration.maximumBatchRecordCount == 0U)
        {
            rejectConfiguration("队列条数上限与单批条数上限都必须至少为 1：0 不是「不限量」而是「一条都不收」");
        }
        for (const HttpClientHeaderField &header: m_configuration.extraHeaders)
        {
            if (header.first.empty())
            {
                rejectConfiguration("附加头部的名字为空：请求行会被撕裂");
            }
            if (hasUnsafeHeaderByte(header.first) || hasUnsafeHeaderByte(header.second))
            {
                rejectConfiguration("附加头部「" + header.first + "」的名字或值里有 CR、LF 或 NUL：那等于在请求里塞进额外一行");
            }
            if (isReservedHeaderName(header.first))
            {
                rejectConfiguration("附加头部占用了「" + header.first + "」：这一项由本出口自己写（正文媒体类型固定是 application/json）");
            }
        }

        HttpOutboundConnectionPool::Config poolConfig;
        poolConfig.idleTimeout              = kOutboundIdleTimeout;
        poolConfig.maximumIdlePerEndpoint   = 1U;
        poolConfig.maximumResponseBodyBytes = kMaximumOtlpResponseBodyBytes;
        m_client                            = std::make_unique<HttpClient>(m_loop, poolConfig);

        // 循环线程最后起：它一起就会读上面全部成员
        m_workerThread = std::jthread([this] { runLoopThread(); });
    }

    OtlpLogSink::~OtlpLogSink()
    {
        // join 之前先让循环有理由停下来；发送协程最多再等一个在途请求的时限
        m_isStopping.store(true, std::memory_order_release);
        m_workerThread.join();
    }

    void OtlpLogSink::write(const Base::LogEvent &event)
    {
        OtlpLogRecord record;
        record.level      = event.level;
        record.timestamp  = event.timestamp;
        record.message    = event.message;
        record.loggerName = std::string{event.loggerNameView()};
        record.threadId   = std::string{event.threadIdView()};
        // 优先读事件自带的上下文：本 Sink 可能被 AsyncSink 包着，写线程的 TLS 是空的
        const Base::LogContextPtr &context = event.context ? event.context : Base::currentLogContext();
        if (context != nullptr)
        {
            for (const auto &[key, value]: context->fields())
            {
                record.attributes.emplace_back(key, value);
            }
        }

        if (m_isStopping.load(std::memory_order_acquire))
        {
            static_cast<void>(m_droppedRecordCount.fetch_add(1, std::memory_order_relaxed));
            return;
        }

        const std::lock_guard lock(m_queueMutex);
        if (m_queue.size() >= m_configuration.maximumPendingRecordCount)
        {
            static_cast<void>(m_droppedRecordCount.fetch_add(1, std::memory_order_relaxed));
            return;
        }
        m_queue.push_back(std::move(record));
    }

    void OtlpLogSink::write(Base::LogEvent &&event)
    {
        // 与左值版本同一份摘取逻辑，只是消息按移动接手（异步队列之外唯一省一次拷贝的地方）
        OtlpLogRecord record;
        record.level      = event.level;
        record.timestamp  = event.timestamp;
        record.message    = std::move(event.message);
        record.loggerName = std::string{event.loggerNameView()};
        record.threadId   = std::string{event.threadIdView()};
        // 优先读事件自带的上下文：本 Sink 可能被 AsyncSink 包着，写线程的 TLS 是空的
        const Base::LogContextPtr &context = event.context ? event.context : Base::currentLogContext();
        if (context != nullptr)
        {
            for (const auto &[key, value]: context->fields())
            {
                record.attributes.emplace_back(key, value);
            }
        }

        if (m_isStopping.load(std::memory_order_acquire))
        {
            static_cast<void>(m_droppedRecordCount.fetch_add(1, std::memory_order_relaxed));
            return;
        }

        const std::lock_guard lock(m_queueMutex);
        if (m_queue.size() >= m_configuration.maximumPendingRecordCount)
        {
            static_cast<void>(m_droppedRecordCount.fetch_add(1, std::memory_order_relaxed));
            return;
        }
        m_queue.push_back(std::move(record));
    }

    void OtlpLogSink::flush()
    {
        // 周期推送没有调用方缓冲可刷；真正要「发完再走」的是析构
    }

    std::uint64_t OtlpLogSink::deliveredBatchCount() const noexcept
    {
        return m_deliveredBatchCount.load(std::memory_order_relaxed);
    }

    std::uint64_t OtlpLogSink::failedBatchCount() const noexcept
    {
        return m_failedBatchCount.load(std::memory_order_relaxed);
    }

    std::uint64_t OtlpLogSink::droppedRecordCount() const noexcept
    {
        return m_droppedRecordCount.load(std::memory_order_relaxed);
    }

    std::size_t OtlpLogSink::pendingRecordCount() const
    {
        const std::lock_guard lock(m_queueMutex);
        return m_queue.size();
    }

    std::string OtlpLogSink::lastFailureReason() const
    {
        const std::lock_guard lock(m_reasonMutex);
        return m_lastFailureReason;
    }

    void OtlpLogSink::runLoopThread()
    {
        // 这条线程自己发起出站连接：Windows 上 Winsock 的初始化引用要自己拿一份（POSIX 是空操作）
        const Platform::Socket::Initialization network;

        // 首次恢复必须在循环线程上：在别的线程 resume 会就地注册观察者，与这里的 run() 抢后端
        Core::Task<void> pumpTask = pump();
        pumpTask.handle().resume();
        m_loop.run();
        m_client.reset();
    }

    Core::Task<void> OtlpLogSink::pump()
    {
        Core::Timer tick{m_loop};
        auto        nextSend = std::chrono::steady_clock::now() + m_configuration.exportInterval;

        for (;;)
        {
            co_await tick.waitFor(kStopPollInterval);
            if (m_isStopping.load(std::memory_order_acquire))
            {
                break;
            }
            if (std::chrono::steady_clock::now() < nextSend)
            {
                continue;
            }
            nextSend = std::chrono::steady_clock::now() + m_configuration.exportInterval;

            std::vector<OtlpLogRecord> batch;
            if (!takeBatch(batch))
            {
                continue;
            }
            if (!co_await sendBatch(batch))
            {
                // 失败留在队列里等下个周期：一次网络故障后面原本是整个队列对着拒绝中的采集端连环撞
                requeue(batch);
            }
        }

        // 收尾：停止收新记录后尽量把队列发完；到期限还没发的按丢弃计，不无限期等
        const auto deadline = std::chrono::steady_clock::now() + m_configuration.shutdownTimeout;
        for (;;)
        {
            std::vector<OtlpLogRecord> batch;
            if (!takeBatch(batch))
            {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                static_cast<void>(m_droppedRecordCount.fetch_add(batch.size(), std::memory_order_relaxed));
                continue;
            }
            if (!co_await sendBatch(batch))
            {
                // 收尾不再重试：期限与在途请求都不允许把进程退出挡在门外
                static_cast<void>(m_droppedRecordCount.fetch_add(batch.size(), std::memory_order_relaxed));
            }
        }
        m_loop.stop();
        co_return;
    }

    bool OtlpLogSink::takeBatch(std::vector<OtlpLogRecord> &batch)
    {
        const std::lock_guard lock(m_queueMutex);
        if (m_queue.empty())
        {
            return false;
        }
        const std::size_t count = std::min(m_configuration.maximumBatchRecordCount, m_queue.size());
        batch.reserve(count);
        for (std::size_t index = 0; index < count; ++index)
        {
            batch.push_back(std::move(m_queue.front()));
            m_queue.pop_front();
        }
        return true;
    }

    void OtlpLogSink::requeue(const std::vector<OtlpLogRecord> &batch)
    {
        const std::lock_guard lock(m_queueMutex);
        if (m_queue.size() + batch.size() > m_configuration.maximumPendingRecordCount)
        {
            // 放不回去就整批丢弃：拆开放会打乱「先发生的先到」，而日志的次序本身就是判据
            static_cast<void>(m_droppedRecordCount.fetch_add(batch.size(), std::memory_order_relaxed));
            return;
        }
        for (auto iterator = batch.rbegin(); iterator != batch.rend(); ++iterator)
        {
            m_queue.push_front(*iterator);
        }
    }

    Core::Task<bool> OtlpLogSink::sendBatch(const std::vector<OtlpLogRecord> &batch)
    {
        const std::string body = formatOtlpLogsJson(m_resource, batch);
        if (body.empty())
        {
            // 非法 UTF-8 让整批成不了形：这一批已经确定不可重试——重排只会每周期再失败一次、
            // 重复计丢弃，还把这批后面的日志堵在队列里。丢弃在这里计一次，按「已处置」交回
            static_cast<void>(m_failedBatchCount.fetch_add(1, std::memory_order_relaxed));
            static_cast<void>(m_droppedRecordCount.fetch_add(batch.size(), std::memory_order_relaxed));
            const std::lock_guard reasonLock(m_reasonMutex);
            m_lastFailureReason = "OTLP 日志出口：一批记录里有非法 UTF-8，整批成不了 JSON";
            co_return true;
        }

        HttpClientRequest request;
        request.method      = "POST";
        request.contentType = kOtlpJsonContentType;
        request.body        = body;
        request.headers     = m_configuration.extraHeaders;

        std::unique_ptr<HttpClientResponse> response;
        std::string                         failureReason;
        try
        {
            response = co_await m_client->send(m_configuration.endpoint, request, m_configuration.requestTimeout);
        } catch (const std::exception &exception)
        {
            failureReason = "OTLP 日志出口：发 POST 时抛出：" + std::string{exception.what()};
        }

        if (failureReason.empty())
        {
            if (response == nullptr)
            {
                failureReason = "OTLP 日志出口：向 " + m_configuration.endpoint + " 发 POST 没能拿到响应（连不上、超时或对端关闭）";
            } else if (response->statusCode < 200 || response->statusCode >= 300)
            {
                failureReason = "OTLP 日志出口：采集端回了状态码 " + std::to_string(response->statusCode);
            }
        }

        if (failureReason.empty())
        {
            static_cast<void>(m_deliveredBatchCount.fetch_add(1, std::memory_order_relaxed));
            co_return true;
        }

        static_cast<void>(m_failedBatchCount.fetch_add(1, std::memory_order_relaxed));
        {
            const std::lock_guard reasonLock(m_reasonMutex);
            m_lastFailureReason = failureReason;
        }
        co_return false;
    }
} // namespace AsynGyanis::Net
