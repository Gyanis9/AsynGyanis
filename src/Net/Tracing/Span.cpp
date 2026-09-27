#include "Net/Tracing/Span.h"

#include "Net/Tracing/Tracer.h"

#include <algorithm>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 一条节起步预留的维度槽位数：HTTP 一节通常挂 5~8 条维度（方法、路由、状态码、字节数、来源），
        /// 不留位就是「1→2→4→8」四次重新分配；留到上限 32 条又等于给每条节先押 1 KiB。
        /// 取 4 是把常见的头几条一次装下，超出部分再按倍增付钱
        constexpr std::size_t kInitialSpanAttributeSlotCount = 4U;
    } // namespace

    std::string_view SpanIdentity::traceIdText() const noexcept
    {
        return std::string_view(traceId.data(), kTraceIdHexDigitCount);
    }

    std::string_view SpanIdentity::spanIdText() const noexcept
    {
        return std::string_view(spanId.data(), kSpanIdHexDigitCount);
    }

    std::string_view SpanIdentity::parentSpanIdText() const noexcept
    {
        // 根节的 parentSpanId 是全 NUL 数组，这里按首个 NUL 截断，交出的是空串而不是 16 个 NUL
        return std::string_view(parentSpanId.data(), hasParent() ? kSpanIdHexDigitCount : 0U);
    }

    bool SpanIdentity::hasParent() const noexcept
    {
        return parentSpanId[0] != '\0';
    }

    Span::Span() noexcept = default;

    Span::Span(std::shared_ptr<Tracer> tracer, SpanIdentity identity, const std::uint8_t flags, const std::uint8_t version, std::string_view name, const SpanKind kind) :
        m_tracer(std::move(tracer)), m_identity(identity), m_flags(flags), m_version(version), m_name(name), m_kind(kind)
    {
        // 只有真在记录的节才打时刻、预留维度表：非记录的替身一条也不会写进去，为它读两次时钟
        // 再取一块内存，就是让每一个「不采」的请求白付一份——采样比例调小省下的钱会被这里吃回去
        if (m_tracer != nullptr)
        {
            m_startMoment = std::chrono::system_clock::now();
            m_steadyStart = std::chrono::steady_clock::now();
            m_attributes.reserve(kInitialSpanAttributeSlotCount);
        }
    }

    Span::~Span()
    {
        finish();
    }

    Span::Span(Span &&other) noexcept :
        m_tracer(std::move(other.m_tracer)), m_identity(other.m_identity), m_flags(other.m_flags), m_version(other.m_version), m_name(std::move(other.m_name)),
        m_kind(other.m_kind), m_startMoment(other.m_startMoment), m_steadyStart(other.m_steadyStart), m_status(other.m_status), m_statusMessage(std::move(other.m_statusMessage)),
        m_attributes(std::move(other.m_attributes)), m_droppedAttributeCount(other.m_droppedAttributeCount), m_isFinished(other.m_isFinished)
    {
        // 原主必须当场失去收口能力：同一节交两次，第二次拿到的名字与维度都已经是 moved-from 的空壳
        other.m_tracer.reset();
        other.m_isFinished = true;
    }

    Span &Span::operator=(Span &&other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        // 先把手上那节收口再换指：覆盖掉一条正在记录的节等于无声丢掉它，
        // 而「换个节去跟踪」这种写法（把 span = tracer->startSpan(...) 用在同一个作用域里）是合法的
        finish();
        m_tracer                = std::move(other.m_tracer);
        m_identity              = other.m_identity;
        m_flags                 = other.m_flags;
        m_version               = other.m_version;
        m_name                  = std::move(other.m_name);
        m_kind                  = other.m_kind;
        m_startMoment           = other.m_startMoment;
        m_steadyStart           = other.m_steadyStart;
        m_status                = other.m_status;
        m_statusMessage         = std::move(other.m_statusMessage);
        m_attributes            = std::move(other.m_attributes);
        m_droppedAttributeCount = other.m_droppedAttributeCount;
        m_isFinished            = other.m_isFinished;

        other.m_tracer.reset();
        other.m_isFinished = true;
        return *this;
    }

    bool Span::isRecording() const noexcept
    {
        return m_tracer != nullptr && !m_isFinished;
    }

    std::string_view Span::name() const noexcept
    {
        return m_name;
    }

    void Span::setName(const std::string_view name)
    {
        if (!isRecording())
        {
            return;
        }
        m_name.assign(name);
    }

    void Span::setAttribute(const std::string_view key, const std::string_view value)
    {
        writeAttribute(key, SpanAttributeValue{std::string(value)});
    }

    void Span::setAttribute(const std::string_view key, const std::int64_t value)
    {
        writeAttribute(key, SpanAttributeValue{value});
    }

    void Span::setStatus(const SpanStatusCode status, const std::string_view message)
    {
        if (!isRecording())
        {
            return;
        }
        // Error 一旦落下就不被 Ok 抹平：一条节里「先失败再补个成功」多半是收尾路径上的误判，
        // 把错误结局藏起来的代价比反过来的方向大
        if (m_status == SpanStatusCode::Error && status == SpanStatusCode::Ok)
        {
            return;
        }
        m_status = status;
        m_statusMessage.assign(message);
    }

    const SpanIdentity &Span::identity() const noexcept
    {
        return m_identity;
    }

    TraceIdentifiers Span::identifiers() const noexcept
    {
        TraceIdentifiers identifiers;
        identifiers.version = m_version;
        identifiers.flags   = m_flags;
        identifiers.traceId = m_identity.traceId;
        // 交给下一跳的 parent-id 就是**本节**的标识：下一跳要把我们当作它的上一节
        identifiers.parentId = m_identity.spanId;
        return identifiers;
    }

    void Span::finish() noexcept
    {
        if (!isRecording())
        {
            return;
        }
        m_isFinished = true;
        // 把引用挪进局部量：万一这是最后一份 Tracer 引用，重置成员就会把编排器当场析构，
        // 而下面那句 accept() 还在用它
        std::shared_ptr<Tracer> tracer = std::move(m_tracer);
        static_cast<void>(tracer->accept(takeRecord()));
    }

    SpanRecord Span::takeRecord() noexcept
    {
        SpanRecord record;
        record.identity              = m_identity;
        record.name                  = std::move(m_name);
        record.kind                  = m_kind;
        record.startMoment           = m_startMoment;
        record.duration              = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_steadyStart);
        record.status                = m_status;
        record.statusMessage         = std::move(m_statusMessage);
        record.attributes            = std::move(m_attributes);
        record.droppedAttributeCount = m_droppedAttributeCount;
        return record;
    }

    void Span::writeAttribute(std::string_view key, SpanAttributeValue value)
    {
        if (!isRecording())
        {
            return;
        }

        // 同名换值要留在原位：维度的顺序按写入顺序报出，替换一条却把它挪到最后会让「两次快照逐行比对」失真
        const auto existing = std::ranges::find(m_attributes, key, &SpanAttribute::key);
        if (existing != m_attributes.end())
        {
            existing->value = std::move(value);
            return;
        }

        if (m_attributes.size() >= kMaximumSpanAttributeCount)
        {
            // 超出上限只丢新来的并计数：OTLP 的 dropped_attributes_count 就是为这件事准备的字段，
            // 让「维度不全」在下游看得出来
            ++m_droppedAttributeCount;
            return;
        }
        m_attributes.push_back(SpanAttribute{.key = std::string(key), .value = std::move(value)});
    }
} // namespace AsynGyanis::Net
