#include "Core/EventLoop/ConnectionDistributor.h"

#include "Base/Log/LogMacros.h"
#include "Platform/IO/FileDescriptor.h"

#include <memory>
#include <utility>

namespace AsynGyanis::Core
{
    namespace
    {
        /**
         * @brief 描述符的交接句柄：没人接手就关闭
         * @details 投递出去的回调可能永远不执行（目标循环先退出，队列里的内容被丢弃），
         *          此时描述符必须有人归还，否则每丢一条连接就漏一个描述符。把描述符包进这个
         *          对象、与回调一起被捕获，回调没跑就由它析构时关闭；接手方 take() 之后
         *          所有权归对方，本对象析构不再重复关闭。
         */
        class HandoffDescriptor
        {
        public:
            explicit HandoffDescriptor(const int fileDescriptor) noexcept :
                m_fileDescriptor(fileDescriptor)
            {
            }

            ~HandoffDescriptor()
            {
                if (Platform::FileDescriptor::isValid(m_fileDescriptor))
                {
                    Platform::FileDescriptor::close(m_fileDescriptor);
                }
            }

            HandoffDescriptor(const HandoffDescriptor &) = delete;

            HandoffDescriptor &operator=(const HandoffDescriptor &) = delete;

            /**
             * @brief 取走描述符并放弃所有权
             * @return int 描述符；本对象此后不再关闭它
             */
            [[nodiscard]] int take() noexcept
            {
                const int fileDescriptor = m_fileDescriptor;
                m_fileDescriptor         = Platform::FileDescriptor::kInvalid;
                return fileDescriptor;
            }

        private:
            int m_fileDescriptor{Platform::FileDescriptor::kInvalid}; ///< 待交接的描述符
        };
    } // namespace

    void ConnectionDistributor::addWorker(EventLoop &loop, Adopter adopter)
    {
        // 空接手动作会让 distribute() 在目标循环上抛 bad_function_call——那是在另一个线程上炸，
        // 属于最难查的一类故障，因此在这里就挡掉
        if (!adopter)
        {
            return;
        }
        m_workers.push_back(Worker{&loop, std::move(adopter)});
    }

    std::size_t ConnectionDistributor::workerCount() const noexcept
    {
        return m_workers.size();
    }

    bool ConnectionDistributor::distribute(const int fileDescriptor) noexcept
    {
        if (m_workers.empty() || !Platform::FileDescriptor::isValid(fileDescriptor))
        {
            return false;
        }

        // 登记顺序即轮转顺序，游标只在本线程（接受循环）里推进
        Worker &worker = m_workers[m_nextWorkerIndex];
        m_nextWorkerIndex = (m_nextWorkerIndex + 1) % m_workers.size();

        // 交接句柄与回调一起投递：目标循环先退出时回调被丢弃，句柄析构把描述符关上。
        // 本函数是 noexcept 而这两步都会分配，因此不能任由分配失败升级成 terminate：
        // - 句柄还没建起来：按「没人接手」返回 false，描述符仍归调用方（由它关闭）；
        // - 句柄一旦建成，所有权就算交出去了——此后一律返回 true，免得调用方再关一次同一个
        //   号（那个号可能已被复用），未执行的投递由句柄自己把它关掉
        std::shared_ptr<HandoffDescriptor> handoff;
        try
        {
            handoff = std::make_shared<HandoffDescriptor>(fileDescriptor);
        } catch (...)
        {
            return false;
        }

        try
        {
            worker.loop->scheduler().postRemote(
                    [adopter = worker.adopter, handoff]() mutable
                    {
                        // take() 之后这个号就没有主人了：adopter 抛出（构造会话失败、bad_alloc）时
                        // 必须就地关掉，否则每失败一次就漏一条描述符——投递「没被执行」由句柄析构
                        // 兜住，而「执行了但抛了」没有任何人会关它
                        const int descriptor = handoff->take();
                        try
                        {
                            adopter(descriptor);
                        } catch (...)
                        {
                            Platform::FileDescriptor::close(descriptor);
                            throw;
                        }
                    });
        } catch (...)
        {
            // 投递排不上队：回调没进队列，句柄随本函数返回析构并关上这条描述符
            LOG_WARN_FMT("ConnectionDistributor: 连接未能交给工作循环（投递无法排队），该连接已关闭");
        }

        ++m_distributedCount;
        return true;
    }

    std::size_t ConnectionDistributor::distributedCount() const noexcept
    {
        return m_distributedCount;
    }
} // namespace AsynGyanis::Core
