#include "Core/Socket/AsyncResolver.h"

#include "Base/Log/LogMacros.h"
#include "Core/EventLoop/EventLoop.h"
#include "Platform/IO/Socket.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace AsynGyanis::Core
{
    namespace
    {
        /**
         * @brief 解析任务的状态：后台线程与协程之间通过它交换结果与句柄
         */
        struct ResolveState
        {
            std::coroutine_handle<>  callerHandle;             ///< 等待结果的协程句柄
            std::atomic<bool>        isCallerAbandoned{false}; ///< 协程帧是否已被销毁（取消、收口）
            std::vector<InetAddress> addresses;                ///< 解析结果

            /// 唤醒等待方：协程帧已被销毁时跳过——resume 一个已释放的帧是释放后使用
            void wakeCaller() const noexcept
            {
                if (!isCallerAbandoned.load(std::memory_order_acquire))
                {
                    callerHandle.resume();
                }
            }
        };

        /**
         * @brief 在后台线程执行阻塞的 getaddrinfo，完成后通过 postRemote 唤醒调用方协程
         */
        void blockingResolve(const std::string host, const uint16_t port, EventLoop *targetLoop,
                             std::shared_ptr<ResolveState> state)
        {
            // Windows 上 getaddrinfo 需要 Winsock 已初始化
            const Platform::Socket::Initialization winsock;
            if (!winsock.isValid())
            {
                targetLoop->scheduler().postRemote([state] { state->wakeCaller(); });
                return;
            }

            addrinfo hints{};
            hints.ai_family   = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags    = AI_ADDRCONFIG;

            const std::string portString = std::to_string(port);

            addrinfo *result = nullptr;
            if (getaddrinfo(host.c_str(), portString.c_str(), &hints, &result) != 0)
            {
                // 解析失败：返回空列表
                targetLoop->scheduler().postRemote([state] { state->wakeCaller(); });
                return;
            }

            // 收集所有地址，先 IPv6 后 IPv4（TcpClient 从列表头部开始试连，因此优先尝试 IPv6）
            std::vector<InetAddress> v6Addresses;
            for (auto *rp = result; rp != nullptr; rp = rp->ai_next)
            {
                if (rp->ai_addr->sa_family == AF_INET6)
                {
                    v6Addresses.emplace_back(*reinterpret_cast<sockaddr_in6 *>(rp->ai_addr));
                } else if (rp->ai_addr->sa_family == AF_INET)
                {
                    state->addresses.emplace_back(*reinterpret_cast<sockaddr_in *>(rp->ai_addr));
                }
            }
            // IPv6 追加在 IPv4 之后 —— 结果列表的头部是 IPv4，尾部是 IPv6
            for (auto &addr: v6Addresses)
            {
                state->addresses.push_back(std::move(addr));
            }
            freeaddrinfo(result);

            // 结果已就绪，唤醒等待的协程
            targetLoop->scheduler().postRemote([state] { state->wakeCaller(); });
        }
    } // namespace

    Task<std::vector<InetAddress>> AsyncResolver::resolve(EventLoop &loop, const std::string_view host, const uint16_t port)
    {
        if (host.empty())
        {
            co_return std::vector<InetAddress>{};
        }

        // state 在协程帧里存活，覆盖整个 co_await 期以及后面的结果读取
        auto state = std::make_shared<ResolveState>();

        // await_suspend 里先存句柄再启线程 —— 保证线程要么看不到 callerHandle 的旧值、
        // 要么看到刚设好的新值，永远不存在「postRemote 跑完但句柄还没设进来」的竞态
        struct ResolveAwaiter
        {
            EventLoop                    &targetLoop;
            std::string                   host;
            uint16_t                      port;
            std::shared_ptr<ResolveState> state;

            bool await_ready() const noexcept { return false; }

            bool await_suspend(const std::coroutine_handle<> handle) noexcept
            {
                state->callerHandle = handle;
                // noexcept 里不能抛出：线程创建失败（句柄/内存耗尽）时返回 false 就地恢复，
                // 结果保持空列表，按文档的「空列表表示解析失败」收尾
                try
                {
                    std::thread worker(blockingResolve, std::move(host), port, &targetLoop, state);
                    worker.detach();
                } catch (...)
                {
                    LOG_WARN("AsyncResolver: 启动解析线程失败（资源耗尽），本次解析按失败返回空地址列表");
                    return false;
                }
                return true;
            }

            void await_resume() const noexcept {}

            ~ResolveAwaiter()
            {
                // 本等待器随协程帧一起析构：帧没了就再也不能被唤醒，
                // 标上标记，让后台线程投回的唤醒跳过 resume（那是释放后使用）
                state->isCallerAbandoned.store(true, std::memory_order_release);
            }
        };

        co_await ResolveAwaiter{loop, std::string(host), port, state};
        co_return std::move(state->addresses);
    }
} // namespace AsynGyanis::Core