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
        const long     ringFileDescriptor = ::syscall(__NR_io_uring_setup, kRingEntryCount, &parameters);
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
            void *const       mapping      = ::mmap(nullptr, combinedSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                                                   m_ringFileDescriptor, IORING_OFF_SQ_RING);
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
            void *const submissionMapping = ::mmap(nullptr, submissionRingSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                                                   m_ringFileDescriptor, IORING_OFF_SQ_RING);
            if (submissionMapping == MAP_FAILED)
            {
                destroy();
                throw Base::SystemException("io_uring 提交队列映射失败");
            }
            m_submissionRingMapping     = submissionMapping;
            m_submissionRingMappingSize = submissionRingSize;

            void *const completionMapping = ::mmap(nullptr, completionRingSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                                                   m_ringFileDescriptor, IORING_OFF_CQ_RING);
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
                ::mmap(nullptr, m_submissionEntriesMappingSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, m_ringFileDescriptor,
                       IORING_OFF_SQES);
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
            m_submissionHead              = std::exchange(other.m_submissionHead, nullptr);
            m_submissionTail              = std::exchange(other.m_submissionTail, nullptr);
            m_submissionRingMask          = std::exchange(other.m_submissionRingMask, nullptr);
            m_submissionEntriesCount      = std::exchange(other.m_submissionEntriesCount, nullptr);
            m_submissionArray             = std::exchange(other.m_submissionArray, nullptr);
            m_completionHead              = std::exchange(other.m_completionHead, nullptr);
            m_completionTail              = std::exchange(other.m_completionTail, nullptr);
            m_completionRingMask          = std::exchange(other.m_completionRingMask, nullptr);
            m_submissionEntries           = std::exchange(other.m_submissionEntries, nullptr);
            m_completionEntries           = std::exchange(other.m_completionEntries, nullptr);
            m_submissionRingMapping       = std::exchange(other.m_submissionRingMapping, nullptr);
            m_submissionRingMappingSize   = std::exchange(other.m_submissionRingMappingSize, 0);
            m_completionRingMapping       = std::exchange(other.m_completionRingMapping, nullptr);
            m_completionRingMappingSize   = std::exchange(other.m_completionRingMappingSize, 0);
            m_submissionEntriesMapping    = std::exchange(other.m_submissionEntriesMapping, nullptr);
            m_submissionEntriesMappingSize = std::exchange(other.m_submissionEntriesMappingSize, 0);
            m_submissionCapacity          = std::exchange(other.m_submissionCapacity, 0);
            m_reservedSubmissionCount     = std::exchange(other.m_reservedSubmissionCount, 0);
            m_ringFileDescriptor          = std::exchange(other.m_ringFileDescriptor, -1);
            m_isValid                     = std::exchange(other.m_isValid, false);
            m_timeoutValue                = std::exchange(other.m_timeoutValue, nullptr);
            m_registrations               = std::move(other.m_registrations);
            m_zombiePolls                 = std::move(other.m_zombiePolls);
            m_inFlightPolls               = std::move(other.m_inFlightPolls);
            m_nextTicket                  = std::exchange(other.m_nextTicket, 1);
            m_timeoutTicket               = std::exchange(other.m_timeoutTicket, 0);
            m_readyEvents                 = std::move(other.m_readyEvents);
        }
        return *this;
    }

    void Uring::destroy()
    {
        // 注册记录只清本地状态：描述符归调用方所有，这里不 close
        m_registrations.clear();
        m_zombiePolls.clear();
        m_inFlightPolls.clear();
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
        // 把 unique_ptr 移到僵尸表：描述符键随之释放，记录仍被持有着（m_inFlightPolls 里那份
        // 裸指针继续有效），等取消完成通知到了再销毁
        const std::uint64_t ticket = registration->inFlightTicket;
        m_zombiePolls.emplace(ticket, std::move(iterator->second));
        m_registrations.erase(iterator);
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

        const unsigned index = m_reservedSubmissionCount & *m_submissionRingMask;
        io_uring_sqe  *const submission = &m_submissionEntries[index];
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

        const std::uint64_t ticket = m_nextTicket++;
        submission->opcode         = IORING_OP_POLL_ADD;
        submission->fd             = registration.fileDescriptor;
        submission->poll32_events  = registration.events;
        submission->user_data      = ticket;

        registration.inFlightTicket = ticket;
        registration.inFlightEvents = registration.events;
        m_inFlightPolls[ticket]     = &registration;
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

        const std::uint64_t ticket = m_nextTicket++;
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
        if (!m_isValid || m_registrations.contains(fileDescriptor))
        {
            return false;
        }

        auto registration           = std::make_unique<Registration>();
        registration->fileDescriptor = fileDescriptor;
        registration->userData       = userData;
        // EPOLLONESHOT 与 EPOLLET 是 epoll 专有位，POLL_ADD 不认：语义由本类用「一次性」承担
        registration->isOneShot      = (events & EPOLLONESHOT) != 0;
        registration->events         = events & ~(EPOLLONESHOT | EPOLLET);

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

        registration->userData  = userData;
        registration->isOneShot = (events & EPOLLONESHOT) != 0;
        const std::uint32_t newEvents = events & ~(EPOLLONESHOT | EPOLLET);
        registration->events = newEvents;

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

    bool Uring::rearmFileDescriptor(const int fileDescriptor, const std::uint32_t events, void *const userData)
    {
        // POLL_ADD 本身就是一次性的：重新武装与修改掩码是同一件事
        return modFileDescriptor(fileDescriptor, events, userData);
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

        const auto pollIterator = m_inFlightPolls.find(ticket);
        if (pollIterator == m_inFlightPolls.end())
        {
            // 取消操作自身的完成、或已被取代的陈旧票据：没有可投递的事件
            return;
        }

        Registration *const registration = pollIterator->second;
        m_inFlightPolls.erase(pollIterator);

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

        if (result >= 0)
        {
            // 轮询结果就是 POLL* 掩码，数值与 EPOLL* 同源
            epoll_event readyEvent{};
            readyEvent.events   = static_cast<std::uint32_t>(result) & (EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDNORM | EPOLLWRNORM);
            readyEvent.data.ptr = registration->userData;
            m_readyEvents.push_back(readyEvent);
        } else if (result != -ECANCELED)
        {
            // 轮询本身失败（描述符被关闭等）：按错误事件上报，让等待方收尾
            epoll_event readyEvent{};
            readyEvent.events   = EPOLLERR | EPOLLHUP;
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
    }

    void Uring::reapCompletions()
    {
        unsigned       head = __atomic_load_n(m_completionHead, __ATOMIC_RELAXED);
        const unsigned tail = __atomic_load_n(m_completionTail, __ATOMIC_ACQUIRE);
        for (; head != tail; ++head)
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

        for (auto &[fileDescriptor, registration]: m_registrations)
        {
            // 取消请求没提交成功过：补一次（删除与重投都依赖它落地）
            if (registration->inFlightTicket != 0 && (registration->pendingDelete || registration->pendingRearm) && !registration->pendingRemove)
            {
                if (submitPollRemove(registration->inFlightTicket))
                {
                    registration->pendingRemove = true;
                }
                continue;
            }
            if (registration->pendingDelete || registration->events == 0 || registration->inFlightTicket != 0)
            {
                continue;
            }
            if (registration->pendingRearm)
            {
                // 取消完成后的重投：成功才清位，失败下次再补
                if (submitPoll(*registration))
                {
                    registration->pendingRearm = false;
                }
                continue;
            }
            if (!registration->isOneShot)
            {
                // 水平触发：原 epoll 会一直上报，这里入睡前补投一次，效果等价
                submitPoll(*registration);
            }
        }
    }

    std::span<epoll_event> Uring::wait(const int timeoutMs)
    {
        m_readyEvents.clear();

        reapCompletions();
        if (!flushSubmissions())
        {
            throw Base::SystemException("io_uring 提交失败");
        }
        maintainRegistrations();
        if (!flushSubmissions())
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
            submitTimeout(timeoutMs);
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

        reapCompletions();
        return {m_readyEvents.data(), m_readyEvents.size()};
    }
} // namespace AsynGyanis::Core

#endif
