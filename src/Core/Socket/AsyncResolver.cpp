#include "Core/Socket/AsyncResolver.h"

#include "Base/Log/LogMacros.h"
#include "Core/EventLoop/EventLoop.h"
#include "Platform/IO/Socket.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AsynGyanis::Core
{
    namespace
    {
        /// 同时在跑的解析线程上限：解析要起线程去跑阻塞的 getaddrinfo，线程栈与内核调度都不免费，
        /// 不设上限的话一次解析风暴能把进程线程数顶到系统限制。到顶之后的解析**如实失败**
        /// （空地址列表）并留一条日志，而不是无限起线程
        constexpr int kMaximumConcurrentResolutions = 256;

        /// 当前在跑的解析数（进程级）
        std::atomic<int> g_activeResolutionCount{0};

        /**
         * @brief 解析结果的缓存有效期
         * @details getaddrinfo 不把记录的 TTL 交出来，所以这个数只能自己定。定短（而不是「一天」这类
         *          常见默认）的理由是「换 IP 的域名要能自己恢复」：一次 DNS 故障、一次重绑定、或一次
         *          运维改记录，最坏情况下也只有这么久在继续用旧地址。定长省下的那几次解析不值钱——
         *          一次解析几毫秒，而用错地址的代价是一次连不上。
         */
        constexpr std::chrono::seconds kCacheTimeToLive{60};

        /// 缓存条数上界：解析必须能继续，缓存不能吃掉内存
        constexpr std::size_t kMaximumCacheEntryCount = 256U;

        /// 一条缓存：解析出来的地址与它的到期时刻
        struct CacheEntry
        {
            std::vector<InetAddress>            addresses;  ///< 解析结果（IPv4 在前、IPv6 在后）
            std::chrono::steady_clock::time_point expiresAt; ///< 到点即视为未命中
        };

        /// 缓存表与其锁：后台解析线程不碰它，只有等待方所在线程读写；但不同事件循环的线程会问同一份表，
        /// 所以必须互斥。锁里只做查与放，不碰任何系统调用
        std::mutex g_cacheMutex;
        std::unordered_map<std::string, CacheEntry> g_addressCache;

        /// 总查询次数与其中命中缓存的次数（进程级）。这是「缓存到底有没有在起作用」的唯一出口，
        /// 用例也靠它的增量来证伪「把缓存撤掉」
        std::atomic<std::uint64_t> g_lookupCount{0};
        std::atomic<std::uint64_t> g_cacheHitCount{0};

        /**
         * @brief 缓存键：主机文本按 ASCII 折小写，端口用单元分隔符接在后面
         * @details 域名大小写无关（RFC 4343），不折叠就会让 Example.COM 与 example.com 各占一格。
         *          分隔符不用 ':'——IPv6 文本（含 IPv4 映射写法 ::ffff:1.2.3.4）里全是冒号，
         *          按它切分会把键切成不唯一的形状。
         */
        std::string cacheKeyOf(const std::string &host, const uint16_t port)
        {
            std::string key;
            key.reserve(host.size() + 8U);
            for (const char character: host)
            {
                key += (character >= 'A' && character <= 'Z') ? static_cast<char>(character + ('a' - 'A')) : character;
            }
            key += '\x1f';
            key += std::to_string(port);
            return key;
        }

        /**
         * @brief 取缓存：过期的条目当场作废
         * @param key 缓存键
         * @return std::optional<std::vector<InetAddress>> 命中时交出存的地址；未命中为空
         */
        std::optional<std::vector<InetAddress>> readCache(const std::string &key)
        {
            const std::lock_guard<std::mutex> guard(g_cacheMutex);
            const auto iterator = g_addressCache.find(key);
            if (iterator == g_addressCache.end())
            {
                return std::nullopt;
            }
            if (std::chrono::steady_clock::now() >= iterator->second.expiresAt)
            {
                g_addressCache.erase(iterator);
                return std::nullopt;
            }
            return iterator->second.addresses;
        }

        /**
         * @brief 写缓存：只存非空结果；表满时先清过期的，仍满就整表丢掉
         * @param key 缓存键
         * @param addresses 本次解析出来的地址
         */
        void writeCache(const std::string &key, const std::vector<InetAddress> &addresses)
        {
            // 空列表把「解析失败」和「这个域名确实没有 A/AAAA 记录」混成一件事，两者都不该粘住
            // 60 秒：一次临时故障被缓存放大成一分钟的连不上，而负向缓存的收益（少问一次）明显更小
            if (addresses.empty())
            {
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            const std::lock_guard<std::mutex> guard(g_cacheMutex);
            if (g_addressCache.size() >= kMaximumCacheEntryCount)
            {
                for (auto iterator = g_addressCache.begin(); iterator != g_addressCache.end();)
                {
                    iterator = iterator->second.expiresAt <= now ? g_addressCache.erase(iterator) : std::next(iterator);
                }
                if (g_addressCache.size() >= kMaximumCacheEntryCount)
                {
                    g_addressCache.clear();
                }
            }
            g_addressCache.insert_or_assign(key, CacheEntry{addresses, now + kCacheTimeToLive});
        }

        /**
         * @brief 一份已占住的解析名额：构造即计一次，析构即还一次
         * @details 名额必须在**发起方线程上同步占用**（见 AsyncResolver::resolve 的 await_suspend），
         *          不能等解析线程起跑后才加计：一批协程在同一个循环线程上连着挂起时，每个发起方读到的
         *          都是「还没到顶」，上限就被整片冲开了。占用凭据跟着 ResolveState 活着，因此解析线程
         *          正常结束、线程没起来、等待方先销毁这三种收场都只归还一次。
         */
        struct ResolutionSlotGuard
        {
            /// 占用一个名额
            ResolutionSlotGuard() noexcept
            {
                g_activeResolutionCount.fetch_add(1, std::memory_order_relaxed);
            }

            /// 归还名额
            ~ResolutionSlotGuard()
            {
                g_activeResolutionCount.fetch_sub(1, std::memory_order_relaxed);
            }

            ResolutionSlotGuard(const ResolutionSlotGuard &) = delete;

            ResolutionSlotGuard &operator=(const ResolutionSlotGuard &) = delete;
        };

        /**
         * @brief 解析任务的状态：后台线程与协程之间通过它交换结果与句柄
         */
        struct ResolveState
        {
            /// 等待结果的协程句柄。**必须是原子的**：置空发生在等待器析构（帧销毁，可能在
            /// 任意线程），读取发生在后台线程投回的唤醒里，两者无同步就是数据竞争；而且
            /// 「先判活再 resume」本身有个窗口——判活通过之后帧仍可能被销毁。改成一取一空
            /// （exchange）：谁取到句柄谁负责恢复，帧销毁时置空则那次恢复自然作废
            std::atomic<std::coroutine_handle<>> callerHandle{nullptr};
            std::vector<InetAddress>             addresses; ///< 解析结果

            /// 本次解析占住的名额：随这份状态一起活到「解析线程与等待方都松手」，那时才归还
            std::shared_ptr<ResolutionSlotGuard> slot;

            /// 唤醒等待方：句柄已被取走或已被置空时什么都不做（resume 已释放的帧是释放后使用）
            void wakeCaller() noexcept
            {
                if (const std::coroutine_handle<> handle = callerHandle.exchange(nullptr, std::memory_order_acq_rel); handle != nullptr)
                {
                    handle.resume();
                }
            }
        };

        /**
         * @brief 把结果交回等待方：先确认还有人等，再碰目标循环
         * @details 投回动作要解引用 `EventLoop`，而循环可能在解析期间已被销毁（等待中的帧随它一起没）。
         *          callerHandle 只在等待器析构时被置空，所以「非空」是「帧还在、因此循环也还在」的
         *          强信号：判空之后再投递，把「整段 getaddrinfo 期间」这个窗口缩到「判空与入队之间」。
         *          要彻底封死它得给循环加一道存活门闩（外部线程持其 shared_ptr 并与析构互斥），
         *          那是跨模块的改动，不在这里顺手做。
         * @note 本函数跑在分离线程上，异常一律不外抛：线程入口没人接就是 std::terminate，
         *       而那时唤醒动作已无从补救，只能记一条日志让现场看得见
         */
        void deliverResult(EventLoop *targetLoop, const std::shared_ptr<ResolveState> &state) noexcept
        {
            if (state->callerHandle.load(std::memory_order_acquire) == nullptr)
            {
                return;
            }
            try
            {
                targetLoop->scheduler().postRemote([state] { state->wakeCaller(); });
            } catch (const std::exception &deliveryError)
            {
                LOG_ERROR_EXCEPTION(deliveryError, "AsyncResolver: 解析结果未能投回事件循环，等待方将带着空结果收尾");
            } catch (...)
            {
                LOG_ERROR("AsyncResolver: 解析结果未能投回事件循环（未知异常），等待方将带着空结果收尾");
            }
        }

        /**
         * @brief 在后台线程执行阻塞的 getaddrinfo，完成后通过 postRemote 唤醒调用方协程
         * @details 本次解析占住的名额挂在 state->slot 上，由这份状态负责归还，本函数不另设计数
         */
        void blockingResolve(const std::string host, const uint16_t port, EventLoop *targetLoop,
                             std::shared_ptr<ResolveState> state)
        {
            // Windows 上 getaddrinfo 需要 Winsock 已初始化
            const Platform::Socket::Initialization winsock;
            if (!winsock.isValid())
            {
                deliverResult(targetLoop, state);
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
                deliverResult(targetLoop, state);
                return;
            }

            // 两趟收集把 IPv6 挪到末尾：返回列表按「IPv4 在前、IPv6 在后」的契约排列
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
            for (auto &address: v6Addresses)
            {
                state->addresses.push_back(std::move(address));
            }
            freeaddrinfo(result);

            deliverResult(targetLoop, state);
        }
        /**
         * @brief 把主机文本按 IP 字面量解析成地址；不是字面量时返回空
         * @param host 主机文本（IPv6 的方括号由上层 URL 解析负责去掉）
         * @param port 端口（主机字节序）
         * @return std::optional<InetAddress> 字面量对应的那个地址
         */
        std::optional<InetAddress> numericLiteralAddress(const std::string &host, const uint16_t port)
        {
            in_addr v4{};
            if (::inet_pton(AF_INET, host.c_str(), &v4) == 1)
            {
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_addr   = v4;
                address.sin_port   = htons(port);
                return InetAddress{address};
            }
            in6_addr v6{};
            if (::inet_pton(AF_INET6, host.c_str(), &v6) == 1)
            {
                sockaddr_in6 address{};
                address.sin6_family = AF_INET6;
                address.sin6_addr   = v6;
                address.sin6_port   = htons(port);
                return InetAddress{address};
            }
            return std::nullopt;
        }
    } // namespace

    Task<std::vector<InetAddress>> AsyncResolver::resolve(EventLoop &loop, std::string host, const uint16_t port)
    {
        if (host.empty())
        {
            co_return std::vector<InetAddress>{};
        }

        // 字面量不进 getaddrinfo。两处理由：
        // ①正确性——hints 里的 AI_ADDRCONFIG 会按「本机有没有配到该族的非回环地址」过滤结果，于是
        //   只有 ::1 可用的容器里连 `[::1]:8080` 都解析不出地址（实测 EAI_ADDRFAMILY），而调用方
        //   已经把地址写在脸上了，没有任何「要不要考虑这台机器支不支持 v6」的余地；
        // ②省一次线程往返——字面量不需要问任何人，起线程跑阻塞调用是白起
        if (const auto literal = numericLiteralAddress(host, port); literal.has_value())
        {
            co_return std::vector<InetAddress>{*literal};
        }

        // 命不命中都要先算键：ResolveAwaiter 会把 host 移走，之后再读它就是空串
        const std::string cacheKey = cacheKeyOf(host, port);
        g_lookupCount.fetch_add(1, std::memory_order_relaxed);
        if (auto cached = readCache(cacheKey); cached.has_value())
        {
            g_cacheHitCount.fetch_add(1, std::memory_order_relaxed);
            co_return *cached;
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
                state->callerHandle.store(handle, std::memory_order_release);

                // 并发上限与线程创建放在同一个 try 里：守卫的构造要分配内存，noexcept 函数里抛出即
                // terminate，而这里两条都是「资源耗尽就按失败收尾」的同一处置
                try
                {
                    // 名额在**发起线程上**就占住（构造即加计），不是等解析线程起跑后才计：一批协程在
                    // 同一个循环线程上连着挂起时，只读计数的发起方全都看到「还没到顶」，上限被整片冲开
                    auto slot = std::make_shared<ResolutionSlotGuard>();
                    if (g_activeResolutionCount.load(std::memory_order_relaxed) > kMaximumConcurrentResolutions)
                    {
                        // 局部 slot 出作用域即归还这一份加计
                        LOG_WARN_FMT("AsyncResolver: 同时在跑的解析已达上限 {}，本次解析按失败返回空地址列表", kMaximumConcurrentResolutions);
                        return false;
                    }

                    // 名额改由状态持有：解析线程与等待方最后松手的那一个负责归还
                    state->slot = std::move(slot);

                    // noexcept 里不能抛出：线程创建失败（句柄/内存耗尽）时返回 false 就地恢复，
                    // 结果保持空列表，按文档的「空列表表示解析失败」收尾
                    std::thread worker(blockingResolve, std::move(host), port, &targetLoop, state);
                    worker.detach();
                } catch (...)
                {
                    LOG_WARN("AsyncResolver: 启动解析线程失败（资源耗尽），本次解析按失败返回空地址列表");
                    // 线程没起来就立刻把名额还回去，不等状态销毁：否则反复失败会把名额耗干，
                    // 让此后的解析永远被上限拒绝
                    state->slot.reset();
                    return false;
                }
                return true;
            }

            void await_resume() const noexcept {}

            ~ResolveAwaiter()
            {
                // 本等待器随协程帧一起析构：帧没了就再也不能被唤醒——把句柄置空，
                // 后台线程投回的唤醒取到空句柄，自然跳过 resume（那是释放后使用）
                state->callerHandle.store(nullptr, std::memory_order_release);
            }
        };

        co_await ResolveAwaiter{loop, std::move(host), port, state};
        writeCache(cacheKey, state->addresses);
        co_return std::move(state->addresses);
    }

    AsyncResolver::Stats AsyncResolver::stats() noexcept
    {
        return Stats{g_lookupCount.load(std::memory_order_relaxed),
                     g_cacheHitCount.load(std::memory_order_relaxed)};
    }
} // namespace AsynGyanis::Core