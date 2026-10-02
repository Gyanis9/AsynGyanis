#include "Net/WebSocket/WebSocketHub.h"

#include "Base/Exception/InvalidArgumentException.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace AsynGyanis::Net
{
    WebSocketSubscription::WebSocketSubscription(WebSocketHub &hub, std::shared_ptr<Detail::WebSocketHubMember> member, const WebSocketSubscriptionId identifier) :
        m_hub(&hub), m_member(std::move(member)), m_id(identifier)
    {
    }

    WebSocketSubscription::~WebSocketSubscription()
    {
        reset();
    }

    // 禁拷贝在头文件里声明为 delete：一份订阅对应表里一个成员，复制出两份就有两处除名点
    WebSocketSubscription::WebSocketSubscription(WebSocketSubscription &&other) noexcept : m_hub(other.m_hub), m_member(std::move(other.m_member)), m_id(other.m_id)
    {
        other.m_hub = nullptr;
        other.m_id  = 0U;
    }

    WebSocketSubscription &WebSocketSubscription::operator=(WebSocketSubscription &&other) noexcept
    {
        if (this != &other)
        {
            reset(); // 自己原先那份要先除名，否则表里留下一条永远没人摘的成员
            m_hub       = other.m_hub;
            m_member    = std::move(other.m_member);
            m_id        = other.m_id;
            other.m_hub = nullptr;
            other.m_id  = 0U;
        }
        return *this;
    }

    void WebSocketSubscription::reset() noexcept
    {
        if (m_hub == nullptr)
        {
            return;
        }
        WebSocketHub *const           hub        = m_hub;
        const WebSocketSubscriptionId identifier = m_id;
        m_hub                                    = nullptr;
        m_id                                     = 0U;
        m_member.reset();
        // 先把自己清干净再动集线器：unsubscribe 不会回到本对象，栈展开路径上也不会二次除名
        hub->unsubscribe(identifier);
    }

    WebSocketSubscriptionId WebSocketSubscription::id() const noexcept
    {
        return m_id;
    }

    bool WebSocketSubscription::isActive() const noexcept
    {
        return m_hub != nullptr;
    }

    WebSocketHub::WebSocketHub(const std::size_t maximumPendingByteCount) : m_maximumPendingByteCount(maximumPendingByteCount)
    {
        if (maximumPendingByteCount == 0U)
        {
            throw Base::InvalidArgumentException("WebSocketHub: 单成员待发队列的字节上界不能为 0，那样一条消息也进不去");
        }
        // 构造时就挂上读数，不等第一次丢弃：面板上「一条没丢」与「这条读数不存在」长得一模一样，
        // 而后者会让运维以为扇出没人盯着
        m_droppedMetric = Core::ProcessMetricsRegistry::registerMetric(
                "asyn_websocket_hub_dropped_messages_total", "因成员待发队列越界而被丢掉的最新消息累计条数（进程内多个集线器求和）", Core::ProcessMetricKind::Counter,
                Core::ProcessMetricMerge::Sum, [this] { return static_cast<std::uint64_t>(m_droppedMessageCount.load(std::memory_order_relaxed)); });
    }

    WebSocketSubscription WebSocketHub::subscribe(const std::string_view topic, WebSocketPeer &peer)
    {
        auto member                              = std::make_shared<Detail::WebSocketHubMember>();
        member->peer                             = &peer;
        const WebSocketSubscriptionId identifier = m_nextIdentifier++;
        m_registrations.push_back(Registration{std::string(topic), member, identifier});
        return WebSocketSubscription(*this, std::move(member), identifier);
    }

    Core::Task<void> WebSocketHub::publish(const std::string_view topic, const std::string_view text)
    {
        co_return co_await publishFrame(topic, text, WebSocketOpCode::Text);
    }

    Core::Task<void> WebSocketHub::publishBinary(const std::string_view topic, const std::string_view payload)
    {
        co_return co_await publishFrame(topic, payload, WebSocketOpCode::Binary);
    }

    Core::Task<void> WebSocketHub::publishFrame(const std::string_view topic, const std::string_view payload, const WebSocketOpCode opCode)
    {
        // 先取一份成员快照再逐个处理：入队与写出都会挂起，这期间表会被别的连接订阅/除名改写，
        // 拿着迭代器遍历就是未定义行为。shared_ptr 保证快照里的成员即使被摘掉也还活着
        std::vector<std::shared_ptr<Detail::WebSocketHubMember>> targets;
        for (const Registration &registration: m_registrations)
        {
            if (registration.topic == topic && registration.member->peer != nullptr)
            {
                targets.push_back(registration.member);
            }
        }

        for (const std::shared_ptr<Detail::WebSocketHubMember> &member: targets)
        {
            if (member->peer == nullptr)
            {
                continue; // 快照之后才被除名（句柄析构或连接收口）：不再往它身上写
            }
            // 一条比整个上界还大的消息永远也放不下：分开判，避免「上界减去长度」在 size_t 上回绕成巨值
            if (payload.size() > m_maximumPendingByteCount || member->pendingByteCount + payload.size() > m_maximumPendingByteCount)
            {
                m_droppedMessageCount.fetch_add(1, std::memory_order_relaxed); // 丢**最新**的一条并计数：已入队的顺序不被插队打乱
                continue;
            }
            member->pendingMessages.push_back(Detail::WebSocketHubPendingMessage{.opCode = opCode, .payload = std::string(payload)});
            member->pendingByteCount += payload.size();

            if (member->isDraining)
            {
                continue; // 已有别的发布协程在替它写：入队即完成，那条 drain 会把这段一起带走
            }
            member->isDraining = true;
            co_await drainMember(member);
        }
    }

    std::size_t WebSocketHub::memberCount(const std::string_view topic) const
    {
        return static_cast<std::size_t>(
                std::ranges::count_if(m_registrations, [topic](const Registration &registration) { return registration.topic == topic && registration.member->peer != nullptr; }));
    }

    std::size_t WebSocketHub::subscriptionCount() const noexcept
    {
        return m_registrations.size();
    }

    std::size_t WebSocketHub::droppedMessageCount() const noexcept
    {
        return m_droppedMessageCount.load(std::memory_order_relaxed);
    }

    std::size_t WebSocketHub::maximumPendingByteCount() const noexcept
    {
        return m_maximumPendingByteCount;
    }

    void WebSocketHub::unsubscribe(const WebSocketSubscriptionId identifier)
    {
        const auto found = std::ranges::find_if(m_registrations, [identifier](const Registration &registration) { return registration.identifier == identifier; });
        if (found == m_registrations.end())
        {
            return; // 已经被摘过：除名必须幂等，异常展开与显式 reset 可能都走到这里
        }
        // 先把对端指针清掉再摘表：正在替它写的 drain 醒来时看到空指针就收手，
        // 而那条连接的拥有者此刻可能正在栈展开中——绝不能再被我们碰
        found->member->peer = nullptr;
        m_registrations.erase(found);
    }

    Core::Task<void> WebSocketHub::drainMember(std::shared_ptr<Detail::WebSocketHubMember> member)
    {
        while (!member->pendingMessages.empty())
        {
            WebSocketPeer *const peer = member->peer;
            if (peer == nullptr || !peer->isOpen())
            {
                // 对端已收口或已被除名：剩下的没有接收者，整队丢掉而不是留在原地长内存
                member->pendingMessages.clear();
                member->pendingByteCount = 0U;
                break;
            }

            Detail::WebSocketHubPendingMessage message = std::move(member->pendingMessages.front());
            member->pendingMessages.pop_front();
            member->pendingByteCount -= message.payload.size();

            // 负载是本地串且在 co_await 期间存活：send* 收的是视图，
            // 而协程要到首次 resume 之后才读入参，交出去之前不能让它失效。
            // 帧类型取自这一条本身：同一条队列里文本与二进制可以交错，取发布者的类型就会发错帧
            bool isSent = false;
            if (message.opCode == WebSocketOpCode::Binary)
            {
                isSent = co_await peer->sendBinary(message.payload);
            } else
            {
                isSent = co_await peer->sendText(message.payload);
            }
            if (!isSent)
            {
                member->pendingMessages.clear();
                member->pendingByteCount = 0U;
                break;
            }
        }
        member->isDraining = false;
    }
} // namespace AsynGyanis::Net
