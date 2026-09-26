#if !ASYN_PLATFORM_WIN32

#include "Core/EventLoop/Uring.h"

#include "Base/Exception/SystemException.h"

#include <linux/io_uring.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <new>
#include <utility>

namespace AsynGyanis::Core
{
    namespace
    {
        /// 环形队列条目数：与 Epoll 侧的事件数组默认容量同档
        constexpr unsigned kRingEntryCount = 1024;

        /// 超时操作票据的标记位：轮询票据的高 32 位是「槽位下标 + 1」，到不了这一位，两者永不撞车
        constexpr std::uint64_t kTimeoutTicketFlag = 1ULL << 63;

        /// 槽位下标在票据里占的位数：低 32 位留给该槽位的代数
        constexpr unsigned kInFlightSlotIndexShift = 32;

        /// 首次分配槽位时的起步格数：与一轮里可能同时在途的轮询数同档，避免逐格长
        constexpr std::size_t kInitialInFlightSlotCount = 64;

        /**
         * @brief 提交一次系统调用形态的 io_uring_enter
         * @return 非负返回值为内核实际消费/唤醒的数量；-1 表示失败（errno 已置位）
         */
        long enterRing(const int ringFileDescriptor, const unsigned toSubmit, const unsigned minimumComplete, const unsigned flags)
        {
            return ::syscall(__NR_io_uring_enter, ringFileDescriptor, toSubmit, minimumComplete, flags, nullptr, 0U);
        }
    } // namespace

    Uring::Uring()
    {
        io_uring_params parameters{};
        const long      ringFileDescriptor = ::syscall(__NR_io_uring_setup, kRingEntryCount, &parameters);
        if (ringFileDescriptor < 0)
        {
            throw Base::SystemException("io_uring_setup 失败（内核可能不支持 io_uring 或被禁用）");
        }
        m_ringFileDescriptor = static_cast<int>(ringFileDescriptor);

        const std::size_t submissionRingSize = parameters.sq_off.array + parameters.sq_entries * sizeof(unsigned);
        const std::size_t completionRingSize = parameters.cq_off.cqes + parameters.cq_entries * sizeof(io_uring_cqe);

        // IORING_FEAT_SINGLE_MMAP：SQ 与 CQ 用同一段映射，分别 mmap 会失败
        if ((parameters.features & IORING_FEAT_SINGLE_MMAP) != 0)
        {
            const std::size_t combinedSize = submissionRingSize > completionRingSize ? submissionRingSize : completionRingSize;
            void *const       mapping      = ::mmap(nullptr, combinedSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, m_ringFileDescriptor, IORING_OFF_SQ_RING);
            if (mapping == MAP_FAILED)
            {
                destroy();
                throw Base::SystemException("io_uring 环形队列映射失败（SQ/CQ 合并段）");
            }
            m_submissionRingMapping     = mapping;
            m_submissionRingMappingSize = combinedSize;
            m_completionRingMapping     = mapping;
            m_completionRingMappingSize = combinedSize;
        } else
        {
            void *const submissionMapping = ::mmap(nullptr, submissionRingSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, m_ringFileDescriptor, IORING_OFF_SQ_RING);
            if (submissionMapping == MAP_FAILED)
            {
                destroy();
                throw Base::SystemException("io_uring 提交队列映射失败");
            }
            m_submissionRingMapping     = submissionMapping;
            m_submissionRingMappingSize = submissionRingSize;

            void *const completionMapping = ::mmap(nullptr, completionRingSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, m_ringFileDescriptor, IORING_OFF_CQ_RING);
            if (completionMapping == MAP_FAILED)
            {
                destroy();
                throw Base::SystemException("io_uring 完成队列映射失败");
            }
            m_completionRingMapping     = completionMapping;
            m_completionRingMappingSize = completionRingSize;
        }

        m_submissionEntriesMappingSize = parameters.sq_entries * sizeof(io_uring_sqe);
        void *const submissionEntriesMapping =
                ::mmap(nullptr, m_submissionEntriesMappingSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, m_ringFileDescriptor, IORING_OFF_SQES);
        if (submissionEntriesMapping == MAP_FAILED)
        {
            destroy();
            throw Base::SystemException("io_uring 提交项数组映射失败");
        }
        m_submissionEntriesMapping = submissionEntriesMapping;

        auto *const submissionBase = static_cast<unsigned char *>(m_submissionRingMapping);
        m_submissionHead           = reinterpret_cast<unsigned *>(submissionBase + parameters.sq_off.head);
        m_submissionTail           = reinterpret_cast<unsigned *>(submissionBase + parameters.sq_off.tail);
        m_submissionRingMask       = reinterpret_cast<unsigned *>(submissionBase + parameters.sq_off.ring_mask);
        m_submissionEntriesCount   = reinterpret_cast<unsigned *>(submissionBase + parameters.sq_off.ring_entries);
        m_submissionArray          = reinterpret_cast<unsigned *>(submissionBase + parameters.sq_off.array);

        auto *const completionBase = static_cast<unsigned char *>(m_completionRingMapping);
        m_completionHead           = reinterpret_cast<unsigned *>(completionBase + parameters.cq_off.head);
        m_completionTail           = reinterpret_cast<unsigned *>(completionBase + parameters.cq_off.tail);
        m_completionRingMask       = reinterpret_cast<unsigned *>(completionBase + parameters.cq_off.ring_mask);
        m_completionEntries        = reinterpret_cast<io_uring_cqe *>(completionBase + parameters.cq_off.cqes);

        m_submissionEntries  = static_cast<io_uring_sqe *>(m_submissionEntriesMapping);
        m_submissionCapacity = parameters.sq_entries;

        // 超时操作的时值必须活到完成通知到达：放在堆上由本对象持有
        m_timeoutValue = new (std::nothrow) __kernel_timespec{};
        if (m_timeoutValue == nullptr)
        {
            destroy();
            throw Base::SystemException("io_uring 超时时值分配失败");
        }

        // 落地缓冲一次定容：此后只 clear 不扩容，交出去的视图不会因下一次 wait() 而悬垂
        m_readyEvents.reserve(kMaximumEventCount);

        m_isValid = true;
    }

    Uring::~Uring()
    {
        destroy();
    }

    Uring::Uring(Uring &&other) noexcept
    {
        *this = std::move(other);
    }

    Uring &Uring::operator=(Uring &&other) noexcept
    {
        if (this != &other)
        {
            destroy();
            m_submissionHead               = std::exchange(other.m_submissionHead, nullptr);
            m_submissionTail               = std::exchange(other.m_submissionTail, nullptr);
            m_submissionRingMask           = std::exchange(other.m_submissionRingMask, nullptr);
            m_submissionEntriesCount       = std::exchange(other.m_submissionEntriesCount, nullptr);
            m_submissionArray              = std::exchange(other.m_submissionArray, nullptr);
            m_completionHead               = std::exchange(other.m_completionHead, nullptr);
            m_completionTail               = std::exchange(other.m_completionTail, nullptr);
            m_completionRingMask           = std::exchange(other.m_completionRingMask, nullptr);
            m_submissionEntries            = std::exchange(other.m_submissionEntries, nullptr);
            m_completionEntries            = std::exchange(other.m_completionEntries, nullptr);
            m_submissionRingMapping        = std::exchange(other.m_submissionRingMapping, nullptr);
            m_submissionRingMappingSize    = std::exchange(other.m_submissionRingMappingSize, 0);
            m_completionRingMapping        = std::exchange(other.m_completionRingMapping, nullptr);
            m_completionRingMappingSize    = std::exchange(other.m_completionRingMappingSize, 0);
            m_submissionEntriesMapping     = std::exchange(other.m_submissionEntriesMapping, nullptr);
            m_submissionEntriesMappingSize = std::exchange(other.m_submissionEntriesMappingSize, 0);
            m_submissionCapacity           = std::exchange(other.m_submissionCapacity, 0);
            m_reservedSubmissionCount      = std::exchange(other.m_reservedSubmissionCount, 0);
            m_ringFileDescriptor           = std::exchange(other.m_ringFileDescriptor, -1);
            m_isValid                      = std::exchange(other.m_isValid, false);
            m_timeoutValue                 = std::exchange(other.m_timeoutValue, nullptr);
            m_registrations                = std::move(other.m_registrations);
            m_zombiePolls                  = std::move(other.m_zombiePolls);
            m_inFlightSlots                = std::move(other.m_inFlightSlots);
            m_freeInFlightSlots            = std::move(other.m_freeInFlightSlots);
            m_attentionDescriptors         = std::move(other.m_attentionDescriptors);
            m_nextTicket                   = std::exchange(other.m_nextTicket, 1);
            m_timeoutTicket                = std::exchange(other.m_timeoutTicket, 0);
            m_readyEvents                  = std::move(other.m_readyEvents);
        }
        return *this;
    }

    void Uring::destroy()
    {
        // 注册记录只清本地状态：描述符归调用方所有，这里不 close
        m_registrations.clear();
        m_zombiePolls.clear();
        m_inFlightSlots.clear();
        m_freeInFlightSlots.clear();
        m_attentionDescriptors.clear();
        m_readyEvents.clear();

        delete m_timeoutValue;
        m_timeoutValue = nullptr;

        if (m_submissionEntriesMapping != nullptr)
        {
            ::munmap(m_submissionEntriesMapping, m_submissionEntriesMappingSize);
            m_submissionEntriesMapping = nullptr;
        }
        if (m_completionRingMapping != nullptr && m_completionRingMapping != m_submissionRingMapping)
        {
            ::munmap(m_completionRingMapping, m_completionRingMappingSize);
        }
        m_completionRingMapping = nullptr;
        if (m_submissionRingMapping != nullptr)
        {
            ::munmap(m_submissionRingMapping, m_submissionRingMappingSize);
            m_submissionRingMapping = nullptr;
        }
        if (m_ringFileDescriptor >= 0)
        {
            ::close(m_ringFileDescriptor);
            m_ringFileDescriptor = -1;
        }
        m_isValid = false;
    }

    Platform::EpollHandle Uring::fileDescriptor() const noexcept
    {
        return m_ringFileDescriptor;
    }

    // ---- 注册表维护 ---------------------------------------------------------

    Uring::Registration *Uring::findRegistration(const int fileDescriptor) const
    {
        const auto iterator = m_registrations.find(fileDescriptor);
        return iterator == m_registrations.end() ? nullptr : iterator->second.get();
    }

    void Uring::eraseRegistration(Registration *const registration)
    {
        const auto iterator = m_registrations.find(registration->fileDescriptor);
        if (iterator != m_registrations.end() && iterator->second.get() == registration)
        {
            m_registrations.erase(iterator);
        }
    }

    void Uring::zombifyRegistration(Registration *const registration)
    {
        const auto iterator = m_registrations.find(registration->fileDescriptor);
        if (iterator == m_registrations.end() || iterator->second.get() != registration)
        {
            return;
        }
        // 把 unique_ptr 移到僵尸表：描述符键随之释放，记录仍被持有着（在途表里那份
        // 裸指针继续有效），等取消完成通知到了再销毁
        const std::uint64_t ticket = registration->inFlightTicket;
        m_zombiePolls.emplace(ticket, std::move(iterator->second));
        m_registrations.erase(iterator);
    }

    // ---- 在途轮询表（票据 → 注册记录） --------------------------------------

    std::uint64_t Uring::reserveInFlightPoll(Registration &registration)
    {
        std::uint32_t slotIndex{0};
        std::uint32_t generation{1};
        if (!m_freeInFlightSlots.empty())
        {
            // 后进先出：刚腾出来的格子大概率还在缓存里，也不用把数组两头都占着
            slotIndex  = m_freeInFlightSlots.back();
            generation = static_cast<std::uint32_t>(m_inFlightSlots[slotIndex].generation + 1U);
            // 代数转回 0 会与「从没用过」撞编码（单条连接复用 40 亿次才会遇到），跳过这个值
            if (generation == 0)
            {
                generation = 1;
            }
            m_inFlightSlots[slotIndex].generation = generation;
            m_inFlightSlots[slotIndex].record     = &registration;
            m_freeInFlightSlots.pop_back();
        } else
        {
            slotIndex = static_cast<std::uint32_t>(m_inFlightSlots.size());
            if (m_inFlightSlots.size() == m_inFlightSlots.capacity())
            {
                // 只有格子用完才倍增一次容量；此后同一批在途条数之内都零分配
                m_inFlightSlots.reserve(m_inFlightSlots.empty() ? kInitialInFlightSlotCount : m_inFlightSlots.capacity() * 2);
            }
            m_inFlightSlots.push_back(InFlightSlot{1, &registration});
        }
        // 下标整体加一再编码：票据 0 因此永远发不出来，而 0 正是「没有在途轮询」的现成哨兵
        return (static_cast<std::uint64_t>(slotIndex + 1U) << kInFlightSlotIndexShift) | generation;
    }

    Uring::Registration *Uring::takeInFlightPoll(const std::uint64_t ticket) noexcept
    {
        const std::uint64_t encodedSlotIndex = ticket >> kInFlightSlotIndexShift;
        // 超时那一路的票据带标记位，解出来的下标必然越界 -> 直接拒，不会错认成某条轮询
        if (encodedSlotIndex == 0 || encodedSlotIndex > m_inFlightSlots.size())
        {
            return nullptr;
        }
        auto &slot = m_inFlightSlots[static_cast<std::size_t>(encodedSlotIndex - 1U)];
        if (slot.record == nullptr || slot.generation != static_cast<std::uint32_t>(ticket & 0xFFFFFFFFU))
        {
            // 格子已还给空闲栈，或代数对不上：取消操作自身的完成、或已被新掩码取代的陈旧票据
            return nullptr;
        }
        Registration *const registration = slot.record;
        slot.record                      = nullptr;
        m_freeInFlightSlots.push_back(static_cast<std::uint32_t>(encodedSlotIndex - 1U));
        return registration;
    }

    // ---- 提交 ---------------------------------------------------------------

    io_uring_sqe *Uring::acquireSubmission()
    {
        // 用「已取走未发布」的本地计数判容量：尾指针此刻还没发布，内核看到的仍是上一批
        if (m_reservedSubmissionCount - __atomic_load_n(m_submissionHead, __ATOMIC_ACQUIRE) >= m_submissionCapacity)
        {
            // 队列满：先提交一批腾位，仍满则放弃（调用方按提交失败处理）
            if (!flushSubmissions())
            {
                return nullptr;
            }
            if (m_reservedSubmissionCount - __atomic_load_n(m_submissionHead, __ATOMIC_ACQUIRE) >= m_submissionCapacity)
            {
                return nullptr;
            }
        }

        const unsigned      index      = m_reservedSubmissionCount & *m_submissionRingMask;
        io_uring_sqe *const submission = &m_submissionEntries[index];
        std::memset(submission, 0, sizeof(io_uring_sqe));
        m_submissionArray[index] = index;
        // 只推进本地计数，**不发布尾指针**：调用方随后才填 opcode/fd 这些字段，
        // 而尾部一旦发布给内核，它就可能读到半写的 SQE（发布统一放在 flushSubmissions）
        ++m_reservedSubmissionCount;
        return submission;
    }

    bool Uring::flushSubmissions()
    {
        // 发布尾指针：此刻所有已取走的槽位都填完了，内核读到的每条 SQE 都是完整的
        const unsigned reserved = m_reservedSubmissionCount;
        unsigned       pending  = reserved - __atomic_load_n(m_submissionHead, __ATOMIC_RELAXED);
        if (pending != 0)
        {
            __atomic_store_n(m_submissionTail, reserved, __ATOMIC_RELEASE);
        }
        while (pending != 0)
        {
            const long submitted = enterRing(m_ringFileDescriptor, pending, 0, 0);
            if (submitted < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                return false;
            }
            pending -= static_cast<unsigned>(submitted);
        }
        return true;
    }

    bool Uring::publishUntilQuiet()
    {
        // 「发布 → 收单」要推到不再产生新的提交项为止。收单会就地往队列里放新提交：
        // 取消完成落到 handleCompletion 的 pendingRearm 分支时，那条「按新掩码重投」是在
        // 收单的过程中取走的，固定两轮就收尾的写法会让它停在队列里。停在队列里等于没发生——
        // 内核没见过这条 SQE，就不会为它产出完成通知，而等待方接下来那次阻塞是 to_submit=0 的
        // 纯等待，谁也不会再来发布它。若它恰好是唯一能叫醒本觉的事件（可写位本就立即可满足，
        // 投上去就会立刻完成），等待方就此睡死。
        //
        // 轮数封顶只为描述符反复抖动时不在本函数里空转；出圈后仍要把已取走的提交项发布出去，
        // 保证「返回时队列里没有悬着的 SQE」这条不变式与轮数无关。
        constexpr unsigned kMaxPublishRounds = 4;
        for (unsigned round = 0; round < kMaxPublishRounds; ++round)
        {
            if (!flushSubmissions())
            {
                return false;
            }
            const unsigned reservedBeforeReap = m_reservedSubmissionCount;
            reapCompletions();
            if (m_reservedSubmissionCount == reservedBeforeReap)
            {
                return true;
            }
        }
        return flushSubmissions();
    }

    bool Uring::submitPoll(Registration &registration)
    {
        if (registration.events == 0)
        {
            // 没有关注位：不投轮询，等 mod 带来新掩码
            return true;
        }

        io_uring_sqe *const submission = acquireSubmission();
        if (submission == nullptr)
        {
            return false;
        }

        // 先占格子：票据本身编码了「哪一格、第几代」，因此不需要另建查找表
        const std::uint64_t ticket = reserveInFlightPoll(registration);
        submission->opcode         = IORING_OP_POLL_ADD;
        submission->fd             = registration.fileDescriptor;
        submission->poll32_events  = registration.events;
        submission->user_data      = ticket;

        registration.inFlightTicket = ticket;
        registration.inFlightEvents = registration.events;
        return true;
    }

    bool Uring::submitPollRemove(const std::uint64_t targetTicket)
    {
        io_uring_sqe *const submission = acquireSubmission();
        if (submission == nullptr)
        {
            return false;
        }
        submission->opcode    = IORING_OP_POLL_REMOVE;
        submission->addr      = targetTicket;
        submission->user_data = 0; // 取消自身的完成不需要票据：查不到就忽略
        return true;
    }

    bool Uring::submitTimeout(const int timeoutMs)
    {
        // 上一次等待遗留的超时还挂着：先撤掉，否则它会在本次等待中途提前唤醒
        if (m_timeoutTicket != 0)
        {
            submitTimeoutRemove(m_timeoutTicket);
            m_timeoutTicket = 0;
        }

        io_uring_sqe *const submission = acquireSubmission();
        if (submission == nullptr)
        {
            return false;
        }

        m_timeoutValue->tv_sec  = timeoutMs / 1000;
        m_timeoutValue->tv_nsec = static_cast<long>(timeoutMs % 1000) * 1000000L;

        const std::uint64_t ticket = kTimeoutTicketFlag | (m_nextTicket++ & 0x00000000FFFFFFFFULL);
        submission->opcode         = IORING_OP_TIMEOUT;
        submission->addr           = reinterpret_cast<std::uint64_t>(m_timeoutValue);
        submission->len            = 1;
        submission->user_data      = ticket;
        m_timeoutTicket            = ticket;
        return true;
    }

    bool Uring::submitTimeoutRemove(const std::uint64_t targetTicket)
    {
        io_uring_sqe *const submission = acquireSubmission();
        if (submission == nullptr)
        {
            return false;
        }
        submission->opcode    = IORING_OP_TIMEOUT_REMOVE;
        submission->addr      = targetTicket;
        submission->user_data = 0;
        return true;
    }

    // ---- 注册接口 -----------------------------------------------------------

    bool Uring::addFileDescriptor(const int fileDescriptor, const std::uint32_t events, void *const userData)
    {
        // 无效描述符当场拒：epoll_ctl 会直接报 EBADF，完成端口那边 CreateIoCompletionPort 也失败。
        // 这边若不拦，POLL_ADD 会带着 -1 进环，内核回一份 POLLERR|POLLHUP 的完成——
        // 于是注册「成功」了，而之后每一轮都有一个凭空冒出来的就绪要交给上层
        if (fileDescriptor < 0 || !m_isValid || m_registrations.contains(fileDescriptor))
        {
            return false;
        }

        auto registration            = std::make_unique<Registration>();
        registration->fileDescriptor = fileDescriptor;
        registration->userData       = userData;
        // EPOLLONESHOT 与 EPOLLET 是 epoll 专有位，POLL_ADD 不认：语义由本类用「一次性」承担
        registration->isOneShot = (events & EPOLLONESHOT) != 0;
        registration->events    = events & ~(EPOLLONESHOT | EPOLLET);

        Registration *const raw = registration.get();
        m_registrations.emplace(fileDescriptor, std::move(registration));
        if (!submitPoll(*raw))
        {
            m_registrations.erase(fileDescriptor);
            return false;
        }
        return true;
    }

    bool Uring::modFileDescriptor(const int fileDescriptor, const std::uint32_t events, void *const userData)
    {
        Registration *const registration = findRegistration(fileDescriptor);
        if (registration == nullptr)
        {
            return false;
        }

        registration->userData        = userData;
        registration->isOneShot       = (events & EPOLLONESHOT) != 0;
        const std::uint32_t newEvents = events & ~(EPOLLONESHOT | EPOLLET);
        registration->events          = newEvents;

        if (registration->inFlightTicket != 0)
        {
            if (newEvents == registration->inFlightEvents)
            {
                // 已按同一掩码武装着：与 epoll 的 MOD 语义一致，无需动作
                return true;
            }
            // 掩码变了：取消在途轮询，完成后按新掩码重投
            if (!registration->pendingRearm)
            {
                registration->pendingRearm = true;
                if (submitPollRemove(registration->inFlightTicket))
                {
                    registration->pendingRemove = true;
                } else
                {
                    // 撤不动就登记下来让维护重试：重投整条链路都挂在这次取消之后，
                    // 没人再提它就等于这条描述符从此不再上报
                    m_attentionDescriptors.push_back(fileDescriptor);
                }
            }
            return true;
        }
        if (registration->pendingDelete)
        {
            return true;
        }
        return submitPoll(*registration);
    }

    bool Uring::delFileDescriptor(const int fileDescriptor)
    {
        Registration *const registration = findRegistration(fileDescriptor);
        if (registration == nullptr)
        {
            return false;
        }

        if (registration->inFlightTicket != 0)
        {
            // 在途轮询先取消：完成通知到齐之前不能销毁记录（内核还持有它的地址）。
            // 但描述符键要**当场**释放——那个 fd 号可能立刻被复用，键留着会让新连接注册失败
            registration->pendingDelete = true;
            if (!registration->pendingRemove && submitPollRemove(registration->inFlightTicket))
            {
                registration->pendingRemove = true;
            }
            zombifyRegistration(registration);
            return true;
        }

        eraseRegistration(registration);
        return true;
    }

    // ---- 完成通知 -----------------------------------------------------------

    void Uring::handleCompletion(const std::uint64_t ticket, const std::int32_t result)
    {
        // 超时操作到点：本次等待结束，不是就绪事件
        if (m_timeoutTicket != 0 && ticket == m_timeoutTicket)
        {
            m_timeoutTicket = 0;
            return;
        }

        Registration *const registration = takeInFlightPoll(ticket);
        if (registration == nullptr)
        {
            // 取消操作自身的完成、或已被取代的陈旧票据：没有可投递的事件
            return;
        }


        if (registration->inFlightTicket != ticket)
        {
            // 该轮询已被更新的掩码取代：结果作废（重投由取消完成的这条路径统一负责）
            return;
        }
        registration->inFlightTicket = 0;
        registration->pendingRemove  = false;

        // 注销中：这次完成只用来清账，结果不再上报——上层的注册对象（IoWatcher）此刻
        // 可能已经析构，投递过去就是释放后使用（Iocp 对同类情形是同一条口径：
        // state.isDeleted 时只清账、不上报）
        if (registration->pendingDelete)
        {
            // 记录本体在僵尸表里（描述符键在 delFileDescriptor 那次就释放了）：这次完成
            // 正是它等的最后一份引用，就地销毁
            m_zombiePolls.erase(ticket);
            return;
        }

        std::uint32_t reportedEvents = 0;
        if (result >= 0)
        {
            // 轮询结果就是 POLL* 掩码，数值与 EPOLL* 同源
            reportedEvents = static_cast<std::uint32_t>(result) & (EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDNORM | EPOLLWRNORM);
        } else if (result != -ECANCELED)
        {
            // 轮询本身失败（描述符被关闭等）：按错误事件上报，让等待方收尾
            reportedEvents = EPOLLERR | EPOLLHUP;
        }

        // 只上报这张记录此刻还要的那些位。提交被推迟到 wait() 才做，于是「关注位已经改掉」与
        // 「内核把旧掩码那份轮询做完了」会撞在一起（epoll 不会——它改掩码当场生效）。不拦的话，
        // 那份没人要的就绪会被 IoWatcher 缓存成「已就绪」，下一次等待凭空醒一次；写侧更糟，
        // 一份陈旧的可写会让人以为缓冲已经排空
        if (registration->events == 0)
        {
            reportedEvents = 0;
        } else
        {
            reportedEvents &= registration->events | EPOLLERR | EPOLLHUP;
        }

        if (reportedEvents != 0)
        {
            epoll_event readyEvent{};
            readyEvent.events   = reportedEvents;
            readyEvent.data.ptr = registration->userData;
            m_readyEvents.push_back(readyEvent);
        }

        if (registration->pendingRearm)
        {
            // 成功才清位：失败留给 maintainRegistrations() 补投，避免等待方永远挂起
            if (submitPoll(*registration))
            {
                registration->pendingRearm = false;
            }
        }

        // 走到这里这条记录手上的轮询已经不算在途了（票据在上面被清掉），可它可能还得重新武装：
        // 水平触发的要补投，重投没成功的要再试。登记下来，别等下一次全表扫描——维护只走登记的。
        if (registration->inFlightTicket == 0 && !registration->pendingDelete && (registration->pendingRearm || (!registration->isOneShot && registration->events != 0)))
        {
            m_attentionDescriptors.push_back(registration->fileDescriptor);
        }
    }

    void Uring::reapCompletions()
    {
        unsigned       head = __atomic_load_n(m_completionHead, __ATOMIC_RELAXED);
        const unsigned tail = __atomic_load_n(m_completionTail, __ATOMIC_ACQUIRE);
        // 一批最多交 kMaximumEventCount 条，与 Epoll/Iocp 同一条口径：取满就停，且头指针不越过
        // 未处理的那条，剩下的完成通知留在 CQ 环里，下一次 wait() 开头先收掉（不丢）。
        // 不设上限时一次突发能把几千条事件塞进同一轮，事件循环要整批派发完才回头取 IO，
        // 尾延迟直接由批大小决定；而且落地缓冲会随之扩容，把上一个视图变成悬垂读。
        for (; head != tail && m_readyEvents.size() < static_cast<std::size_t>(kMaximumEventCount); ++head)
        {
            const io_uring_cqe *const completion = &m_completionEntries[head & *m_completionRingMask];
            handleCompletion(completion->user_data, completion->res);
        }
        // 完成项消费完再推进头指针：内核据此判断环形空间可复用
        __atomic_store_n(m_completionHead, head, __ATOMIC_RELEASE);
    }

    void Uring::maintainRegistrations()
    {
        // 僵尸记录先补取消：它们已经不在 m_registrations 里，取消请求没提交成功的话
        // 永远等不到完成通知，记录就永远留在僵尸表里
        for (auto &[ticket, registration]: m_zombiePolls)
        {
            if (!registration->pendingRemove && submitPollRemove(ticket))
            {
                registration->pendingRemove = true;
            }
        }

        // 只处理登记过要动作的那几条。早先的写法是把整张注册表走一遍，空闲的在途轮询虽然
        // 什么都不做也要被看一眼——那是 O(在册描述符数) 的一趟，而事件循环每收一批事件都要
        // 走一次：实测单次 wait(0) 从 128 条的 12 微秒涨到 4096 条的 405 微秒，而同形状的
        // epoll 后端是平的（约 0.13 微秒）。连接数是倒数级别的吞吐损失，绝不能留在循环里。
        //
        // 只处理进函数时已有的条目：处理过程中提交失败会重新登记到表尾，那部分留给下一轮，
        // 就地抹掉已处理的前缀则让这张表的容量稳定（swap 到局部会把缓冲一起丢掉）
        const std::size_t registeredCount = m_attentionDescriptors.size();
        for (std::size_t index = 0; index < registeredCount; ++index)
        {
            const int           fileDescriptor   = m_attentionDescriptors[index];
            Registration *const registration     = findRegistration(fileDescriptor);
            bool                needsAnotherPass = false;

            if (registration == nullptr)
            {
                // 记录已经不在了：注销路径把它移进了僵尸表（那边自己会补撤），或直接销毁
                continue;
            }
            if (registration->inFlightTicket != 0)
            {
                // 取消请求没提交成功过：补一次（删除与重投都依赖它落地）
                if ((registration->pendingDelete || registration->pendingRearm) && !registration->pendingRemove)
                {
                    if (submitPollRemove(registration->inFlightTicket))
                    {
                        registration->pendingRemove = true;
                    } else
                    {
                        needsAnotherPass = true;
                    }
                }
            } else if (!registration->pendingDelete && registration->events != 0 && (registration->pendingRearm || !registration->isOneShot))
            {
                // 没武装的两种活：取消完成后的按新掩码重投（成功才清位，失败下次再补），
                // 以及水平触发的重新武装——原 epoll 会一直上报，这里入睡前补投一次，效果等价
                if (submitPoll(*registration))
                {
                    registration->pendingRearm = false;
                } else
                {
                    needsAnotherPass = true;
                }
            }
            if (needsAnotherPass)
            {
                m_attentionDescriptors.push_back(fileDescriptor);
            }
        }
        m_attentionDescriptors.erase(m_attentionDescriptors.begin(), m_attentionDescriptors.begin() + static_cast<std::ptrdiff_t>(registeredCount));
    }

    std::span<epoll_event> Uring::wait(const int timeoutMs)
    {
        m_readyEvents.clear();

        // 上一觉留下的完成先收掉：它们要在本轮交付，也让下面的维护少做无用判断
        reapCompletions();
        maintainRegistrations();
        // 入睡前把「已取走的提交」全交给内核，并把因此就绪的完成收回来——publishUntilQuiet()
        // 两件事一起担保：睡前不留悬着的 SQE，且 wait(0) 的「本轮能收的都收掉」成立
        if (!publishUntilQuiet())
        {
            throw Base::SystemException("io_uring 提交失败");
        }

        if (!m_readyEvents.empty())
        {
            return {m_readyEvents.data(), m_readyEvents.size()};
        }
        if (timeoutMs == 0)
        {
            // 非阻塞：已把当前能收的完成都收掉，就此返回
            return {};
        }

        if (timeoutMs > 0)
        {
            // 时限操作的返回值不能吞：没投上这一觉就没有期限（下面的 enter 是「至少等一条完成」），
            // 调用方会一直睡到某个无关事件把它叫醒。与其挂着或交空结果让上层空转，
            // 不如与同一函数里「提交失败」的既有口径一致地报出来
            if (!submitTimeout(timeoutMs))
            {
                throw Base::SystemException("io_uring 提交超时操作失败（提交队列拿不到位置）：本次等待无法设定时限，宁可失败也不无限睡");
            }
            if (!flushSubmissions())
            {
                throw Base::SystemException("io_uring 提交超时操作失败");
            }
        }

        // 阻塞等至少一条完成（超时到点也算一条）
        while (true)
        {
            const long waited = enterRing(m_ringFileDescriptor, 0, 1, IORING_ENTER_GETEVENTS);
            if (waited < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                throw Base::SystemException("io_uring 等待完成失败");
            }
            break;
        }

        // 醒来这一趟同样不许把重投悬着留给下一觉：被叫醒的那条完成可能就是某个取消完成，
        // handleCompletion 会就地按新掩码重投，而调用方完全可能马上又进到无限等待里
        if (!publishUntilQuiet())
        {
            throw Base::SystemException("io_uring 提交失败");
        }
        return {m_readyEvents.data(), m_readyEvents.size()};
    }
} // namespace AsynGyanis::Core

#endif
