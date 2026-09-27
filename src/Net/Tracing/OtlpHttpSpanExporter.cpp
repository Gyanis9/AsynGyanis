#include "Net/Tracing/OtlpHttpSpanExporter.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Coroutine/Task.h"
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

        /// 本出口自己占用的头部名：调用方再给一份就会出现两条同名头部
        constexpr std::string_view kReservedHeaderNames[] = {"host", "content-length", "connection", "content-type"};

        /// @brief 抛出一条配置不成立的中文原因（带字段名与替代做法）
        [[noreturn]] void rejectConfiguration(const std::string &reason)
        {
            throw Base::InvalidArgumentException("OTLP 出口无法启动：" + reason);
        }

        /**
         * @brief 把配置里的采集端地址拆成 ParsedUrl，空与畸形都在此刻拒掉
         * @details 拆 URL 的活交给 parseUrl，但报错要落到本出口的字段上：直接把它那句
         *          「URL 里没有主机部分」抛出去，调用方看不出自己填错的是哪个配置项。
         * @param endpoint 配置给出的地址
         * @return ParsedUrl 拆好的目标
         * @throws Base::InvalidArgumentException 地址为空，或 parseUrl 判它不成形
         */
        ParsedUrl makeTarget(const std::string &endpoint)
        {
            if (endpoint.empty())
            {
                rejectConfiguration("采集端地址为空。请在 OtlpHttpSpanExporter::Configuration::endpoint 里给出"
                                    "形如 http(s)://collector:4318/v1/traces 的地址");
            }
            try
            {
                return parseUrl(endpoint);
            } catch (const Base::InvalidArgumentException &error)
            {
                rejectConfiguration("采集端地址「" + endpoint + "」不是一枚可用的 http(s) URL：" + error.what() + "。请写成 http(s)://host:port/v1/traces");
            }
        }

        /// @brief 两个头部名忽略大小写是否相同（头部名按 ASCII 折，不走 locale）
        bool isSameHeaderNameIgnoringCase(const std::string_view left, const std::string_view right)
        {
            if (left.size() != right.size())
            {
                return false;
            }
            const auto foldCharacter = [](const char character) { return character >= 'A' && character <= 'Z' ? static_cast<char>(character + 0x20) : character; };
            for (std::size_t index = 0; index < left.size(); ++index)
            {
                if (foldCharacter(left[index]) != foldCharacter(right[index]))
                {
                    return false;
                }
            }
            return true;
        }

        /// @brief 头部名是否属于「本出口自己会写」的那几个（大小写无关）
        bool isReservedHeaderName(const std::string_view name)
        {
            return std::ranges::any_of(kReservedHeaderNames, [&](const std::string_view reserved) { return isSameHeaderNameIgnoringCase(reserved, name); });
        }

        /// @brief 头部名或值里有没有会撕裂请求行的字节（CR、LF 与 NUL）
        bool hasUnsafeHeaderByte(const std::string_view text)
        {
            return std::ranges::any_of(text, [](const char character) { return character == '\r' || character == '\n' || character == '\0'; });
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
        for (const HttpClientHeaderField &header: m_configuration.extraHeaders)
        {
            if (header.first.empty())
            {
                rejectConfiguration("附加头部的名字为空：请求行会被撕裂。请给一个非空的头部名");
            }
            if (hasUnsafeHeaderByte(header.first) || hasUnsafeHeaderByte(header.second))
            {
                rejectConfiguration("附加头部「" + header.first + "」的名字或值里有 CR、LF 或 NUL：那等于在请求里塞进额外一行");
            }
            if (isReservedHeaderName(header.first))
            {
                rejectConfiguration("附加头部占用了「" + header.first +
                                    "」：这一项由本出口自己写"
                                    "（正文媒体类型固定是 application/json），再给一份就会出现两条同名头部");
            }
        }

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
        Core::Task<void> pump = pumpBatches();
        static_cast<void>(pump.handle().resume());
        m_loop.run();
        // HttpClient 与它的池都归属这条循环，必须在 run() 返回之后、仍在本线程上收掉
        m_client.reset();
    }

    Core::Task<void> OtlpHttpSpanExporter::pumpBatches()
    {
        HttpClient &client = *m_client;
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

                std::unique_ptr<HttpClientResponse> response;
                std::string                         failureReason;
                try
                {
                    response = co_await client.send(m_configuration.endpoint, request, m_configuration.requestTimeout);
                } catch (const std::exception &exception)
                {
                    // 出站这一段的异常不能让发送协程带出去：那会顺着 run() 打断整条循环线程。
                    // 这一批按没送达计，原因留给 lastFailureReason()
                    failureReason = "OTLP 出口：发 POST 时抛出：" + std::string{exception.what()};
                }

                if (failureReason.empty())
                {
                    // 三段判定写成显式分支：空串在这里的含义是「送达到且 2xx」，与「没拿到响应」
                    // 与「拿到非 2xx」三种结果都要能被读者一眼分开
                    if (response == nullptr)
                    {
                        failureReason = "OTLP 出口：向 " + m_configuration.endpoint + " 发 POST 没能拿到响应（连不上、超时或对端关闭）";
                    } else if (response->statusCode < 200 || response->statusCode >= 300)
                    {
                        failureReason = "OTLP 出口：采集端回了状态码 " + std::to_string(response->statusCode);
                    }
                }

                if (failureReason.empty())
                {
                    static_cast<void>(m_deliveredBatchCount.fetch_add(1, std::memory_order_relaxed));
                } else
                {
                    static_cast<void>(m_failedBatchCount.fetch_add(1, std::memory_order_relaxed));
                    const std::lock_guard reasonLock(m_reasonMutex);
                    m_lastFailureReason = failureReason;
                }
            }

            {
                // 队列排空且不再收新批：这就是 shutdown() 在等的那声「发完了」
                const std::lock_guard lock(m_stateMutex);
                if (!m_isAccepting)
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
