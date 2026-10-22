#include "Net/Tracing/OtlpHttpSpanExporter.h"

#include "Net/Telemetry/OtlpHttpExportSupport.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Tls/TlsPolicy.h"
#include "Platform/IO/Socket.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 采集端响应的正文上限：OTLP 的回复只可能是一份很小的 partial_success JSON
        constexpr std::size_t kMaximumOtlpResponseBodyBytes = 64U * 1024U;

        /// 那条到采集端的连接空闲多久就收掉：留着一条长期不用的连接不如让对端说了算
        constexpr std::chrono::milliseconds kOutboundIdleTimeout{30000};


        /// 一次失败之后至少要隔多久再试：连不上与被拒都不给「趁对端拒绝的当口把队列撞光」留窗口
        constexpr std::chrono::milliseconds kFailurePauseFloor{200};

        /// 对端要求等多久，本出口最多就等多久；越界的 `Retry-After` 按这个数钳住（见 retryPauseFor）
        constexpr std::chrono::milliseconds kMaximumRetryPause{5000};

        /**
         * @brief 这一次失败之后该等多久再试下一批
         * @details `Retry-After` 只在 429 与 503 上有定义（RFC 9110 §10.2.3），因此只在这两个状态码上读它，
         *          读到了就照对端说的等，但封顶在 kMaximumRetryPause：那条头合法地写成「86400」时，
         *          照单全收等于让链路在两小时里静默攒着一队列必然被丢掉的节。没有这条头、写法读不懂、
         *          或者压根没拿到响应都退回 kFailurePauseFloor——把「读不懂」折成 0 秒是最坏的一种省事，
         *          那正是「现在就再撞一次」的意思。
         * @param response 这次拿到的响应；连不上或超时时为空
         * @return std::chrono::milliseconds 退避时长
         */
        std::chrono::milliseconds retryPauseFor(const HttpClientResponse *response)
        {
            if (response != nullptr && (response->statusCode == 429 || response->statusCode == 503))
            {
                if (const auto retryAfter = response->retryAfter(); retryAfter.has_value())
                {
                    // 先钳「秒」这一档的数，再做秒→毫秒那次乘法：`Retry-After` 写成
                    // `9223372036854775807` 是一条语法完全合法的头（RFC 9110 §10.1.2 只要求若干个十进制
                    // 数字），而 duration_cast 会把它乘 1000——有符号溢出是 UB，它在本框架里的落法是折成
                    // 一个负数，随后 clamp 取地板 200 毫秒：对端明确说了「限流」，我们反倒把重试频率抬到最高
                    const std::int64_t cappedSeconds = std::clamp(retryAfter->count(), static_cast<std::int64_t>(0), static_cast<std::int64_t>(kMaximumRetryPause.count() / 1000));
                    return std::clamp(std::chrono::milliseconds{cappedSeconds * 1000}, kFailurePauseFloor, kMaximumRetryPause);
                }
            }
            return kFailurePauseFloor;
        }

        /// @brief 抛出一条配置不成立的中文原因（带字段名与替代做法）
        [[noreturn]] void rejectConfiguration(const std::string &reason)
        {
            throw Base::InvalidArgumentException("OTLP 出口无法启动：" + reason);
        }


        /// @brief 把采集端地址拆成 ParsedUrl，错误前缀与本出口的配置字段在这里定死
        ParsedUrl makeTarget(const std::string &endpoint)
        {
            return Telemetry::makeOtlpTarget(endpoint, "OTLP 出口无法启动：", "OtlpHttpSpanExporter::Configuration::endpoint", "/v1/traces");
        }
    } // namespace

    OtlpHttpSpanExporter::OtlpHttpSpanExporter(Configuration configuration) : OtlpHttpSpanExporter(std::move(configuration), Core::TlsPolicy{})
    {
    }

    OtlpHttpSpanExporter::OtlpHttpSpanExporter(Configuration configuration, const Core::TlsPolicy &tlsPolicy) :
        m_configuration(std::move(configuration)), m_target(makeTarget(m_configuration.endpoint))
    {
        if (m_configuration.maximumPendingBatchCount == 0U)
        {
            rejectConfiguration("待发送队列的批数上界为 0：那不是「不限量」而是「一条都不收」。请至少给 1");
        }
        if (m_configuration.requestTimeout <= std::chrono::milliseconds::zero() || m_configuration.shutdownTimeout <= std::chrono::milliseconds::zero())
        {
            rejectConfiguration("单次请求时限与收尾期限都必须大于 0，否则发送会没有终点、收尾会永远等下去");
        }
        Telemetry::validateOtlpExtraHeaders(m_configuration.extraHeaders, "OTLP 出口无法启动：");

        HttpOutboundConnectionPool::Config poolConfig;
        poolConfig.idleTimeout              = kOutboundIdleTimeout;
        poolConfig.maximumIdlePerEndpoint   = 1U; ///< 只对一个采集端发，留一条就够
        poolConfig.maximumResponseBodyBytes = kMaximumOtlpResponseBodyBytes;
        // 客户端在构造期就建好：TLS 策略被 OpenSSL 拒绝时要从这里抛出，而不是等第一条发不出去的批
        m_client = std::make_unique<HttpClient>(m_loop, poolConfig, tlsPolicy);

        // 循环线程最后起：它一起就会读上面全部成员
        m_workerThread = std::jthread([this] { runLoopThread(); });
    }

    OtlpHttpSpanExporter::~OtlpHttpSpanExporter()
    {
        // 成员析构顺序在声明的末尾（m_workerThread 最后声明、最先析构），jthread 会在这里 join；
        // 但 join 之前得先让那条循环有理由停下来，否则 run() 永不返回、这里就永远等下去
        shutdown();
    }

    bool OtlpHttpSpanExporter::exportSpans(const TraceResource &resource, const std::vector<SpanRecord> &spans)
    {
        std::string body = formatOtlpTracesJson(resource, spans);
        if (body.empty())
        {
            static_cast<void>(m_droppedBatchCount.fetch_add(1, std::memory_order_relaxed));
            {
                const std::lock_guard lock(m_reasonMutex);
                m_lastFailureReason = "OTLP 出口：这批的正文没能成形（取值里有进不了 JSON 的字节），整批丢掉";
            }
            return false;
        }

        {
            const std::lock_guard lock(m_stateMutex);
            if (!m_isAccepting)
            {
                static_cast<void>(m_droppedBatchCount.fetch_add(1, std::memory_order_relaxed));
                return false;
            }
            if (m_queue.size() >= m_configuration.maximumPendingBatchCount)
            {
                // 只丢新到的：队列前面那些已经等了更久，再剪掉开头会让一批彻底没机会送达
                static_cast<void>(m_droppedBatchCount.fetch_add(1, std::memory_order_relaxed));
                return false;
            }
            m_queue.push_back(PendingBatch{.body = std::move(body)});
        }
        wakePump();
        return true;
    }

    std::string_view OtlpHttpSpanExporter::exporterName() const noexcept
    {
        return "otlp-http";
    }

    void OtlpHttpSpanExporter::shutdown() noexcept
    {
        {
            const std::lock_guard lock(m_stateMutex);
            m_isAccepting = false;
        }
        // 叫醒它一次：挂着的协程否则要到下一次投递才会醒，而收尾不该依赖「还有下一条」
        wakePump();
        {
            std::unique_lock lock(m_stateMutex);
            static_cast<void>(m_drainedCondition.wait_for(lock, m_configuration.shutdownTimeout, [this] { return m_isDrained; }));
            if (!m_isDrained)
            {
                // 期限到了还压着（或对端迟迟不应答）：那几批按丢弃计，不能让进程退出无门
                static_cast<void>(m_droppedBatchCount.fetch_add(m_queue.size(), std::memory_order_relaxed));
                m_queue.clear();
            }
        }
        // EventLoop::stop() 本身线程安全；返回后那条线程会自己把 run() 收尾
        m_loop.stop();
    }

    std::uint64_t OtlpHttpSpanExporter::deliveredBatchCount() const noexcept
    {
        return m_deliveredBatchCount.load(std::memory_order_relaxed);
    }

    std::uint64_t OtlpHttpSpanExporter::failedBatchCount() const noexcept
    {
        return m_failedBatchCount.load(std::memory_order_relaxed);
    }

    std::uint64_t OtlpHttpSpanExporter::droppedBatchCount() const noexcept
    {
        return m_droppedBatchCount.load(std::memory_order_relaxed);
    }

    std::size_t OtlpHttpSpanExporter::pendingBatchCount() const noexcept
    {
        const std::lock_guard lock(m_stateMutex);
        return m_queue.size();
    }

    std::string OtlpHttpSpanExporter::lastFailureReason() const
    {
        const std::lock_guard lock(m_reasonMutex);
        return m_lastFailureReason;
    }

    void OtlpHttpSpanExporter::runLoopThread()
    {
        // 这条线程自己发起出站连接：Windows 上 Winsock 的初始化引用要自己拿一份，
        // 不能靠主线程那一份的存续（POSIX 上这是空操作）
        const Platform::Socket::Initialization network;

        // 发送协程首次恢复必须在循环线程上：在调用线程 resume 会让它就地注册观察者，
        // 与这里的 run() 抢同一份后端。帧留在本函数的栈上，活到 run() 返回之后
        {
            Core::Task<void> pump = pumpBatches();
            static_cast<void>(pump.handle().resume());
            m_loop.run();
            // 先销毁发送协程、再收客户端：协程帧里的出向并发闸守卫要在池子仍活着时退账。
            // 反过来（先 reset 客户端）会让守卫在已释放的池子上调 releaseInFlight
        }
        // HttpClient 与它的池都归属这条循环，必须在 run() 返回之后、仍在本线程上收掉
        m_client.reset();
    }

    Core::Task<void> OtlpHttpSpanExporter::pumpBatches()
    {
        HttpClient &client = *m_client;
        // 退避定时器只是本循环的定时队列上一个句柄，构造与析构都不产生系统调用，因此放在协程帧里
        Core::Timer backoffTimer{m_loop};
        for (;;)
        {
            co_await WakeupAwaiter{*this};

            PendingBatch batch;
            while (takeNextBatch(batch))
            {
                HttpClientRequest request;
                request.method      = "POST";
                request.contentType = kOtlpJsonContentType;
                // 视图指向本协程帧里的局部量：它要活到 co_await 返回，而 batch 正好活过这一次挂起
                request.body    = batch.body;
                request.headers = m_configuration.extraHeaders;

                const Telemetry::OtlpHttpExportAttempt attempt =
                        co_await Telemetry::sendOtlpHttpRequest(client, m_configuration.endpoint, request, m_configuration.requestTimeout, "OTLP 出口");

                if (attempt.failureReason.empty())
                {
                    static_cast<void>(m_deliveredBatchCount.fetch_add(1, std::memory_order_relaxed));
                } else
                {
                    static_cast<void>(m_failedBatchCount.fetch_add(1, std::memory_order_relaxed));
                    {
                        const std::lock_guard reasonLock(m_reasonMutex);
                        m_lastFailureReason = attempt.failureReason;
                    }

                    // 失败就停在这一次，剩下的批次留在队列里：一次网络故障后面跟着的原本是整个队列
                    // 对着正在拒绝的采集端连环撞光，而撞光的每一批都只能按失败计——它们本可以在对端
                    // 缓过来之后送达。退避时长见 retryPauseFor()；收尾期间照同一节奏走，整段等待仍被
                    // shutdownTimeout 兜住，不会因此把进程退出挡在门外
                    co_await backoffTimer.waitFor(retryPauseFor(attempt.response.get()));
                    break;
                }
            }

            {
                // 队列排空且不再收新批：这就是 shutdown() 在等的那声「发完了」。
                // 「排空」这一半必须自己判：失败退避那条出口离开内层循环时队列可以还有货，
                // 只按 !m_isAccepting 就会把「还剩几条没发」报成「发完了」，那几批既不计数也没人再管
                const std::lock_guard lock(m_stateMutex);
                if (!m_isAccepting && m_queue.empty())
                {
                    m_isDrained  = true;
                    m_parkedPump = {};
                    m_drainedCondition.notify_all();
                    co_return;
                }
            }
        }
    }

    bool OtlpHttpSpanExporter::takeNextBatch(PendingBatch &destination)
    {
        const std::lock_guard lock(m_stateMutex);
        if (m_queue.empty())
        {
            return false;
        }
        destination = std::move(m_queue.front());
        m_queue.pop_front();
        return true;
    }

    bool OtlpHttpSpanExporter::hasWorkOrStopped() const noexcept
    {
        const std::lock_guard lock(m_stateMutex);
        return !m_queue.empty() || !m_isAccepting;
    }

    bool OtlpHttpSpanExporter::parkUnlessWorkArrived(const std::coroutine_handle<> handle) noexcept
    {
        const std::lock_guard lock(m_stateMutex);
        // 存句柄的这一拍再判一次：生产者可能正好落在「判定没活」与「把句柄存下」之间，
        // 那一拍叫醒的手就没人接，队列里那条批会一直等到下一次投递
        if (!m_queue.empty() || !m_isAccepting)
        {
            return false;
        }
        m_parkedPump = handle;
        return true;
    }

    void OtlpHttpSpanExporter::wakePump() noexcept
    {
        std::coroutine_handle<> handle;
        {
            const std::lock_guard lock(m_stateMutex);
            handle       = m_parkedPump;
            m_parkedPump = {};
        }
        if (handle)
        {
            // 跨线程恢复协程只走这一条通道：它把唤醒送进循环自己的就绪队列
            m_loop.scheduler().scheduleRemote(handle);
        }
    }

    bool OtlpHttpSpanExporter::WakeupAwaiter::await_ready() const noexcept
    {
        return m_exporter.hasWorkOrStopped();
    }

    bool OtlpHttpSpanExporter::WakeupAwaiter::await_suspend(const std::coroutine_handle<> handle) noexcept
    {
        // 协议是 true = 保持挂起、false = 就地继续跑：句柄登记成功才该交回 true。
        // 反过来写会把「已经挂起」当成「没挂起」，同一帧被叫醒时再恢复一次——直接跳进垃圾地址
        return m_exporter.parkUnlessWorkArrived(handle);
    }
} // namespace AsynGyanis::Net
