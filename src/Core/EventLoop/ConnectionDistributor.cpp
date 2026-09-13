#include "Core/EventLoop/ConnectionDistributor.h"

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

        // 轮转取一个工作循环：登记顺序即轮转顺序，游标只在本线程（接受循环）里推进
        Worker &worker = m_workers[m_nextWorkerIndex];
        m_nextWorkerIndex = (m_nextWorkerIndex + 1) % m_workers.size();
        ++m_distributedCount;

        // 交接句柄与回调一起投递：目标循环先退出时回调被丢弃，句柄析构把描述符关上
        auto handoff = std::make_shared<HandoffDescriptor>(fileDescriptor);
        worker.loop->scheduler().postRemote(
                [adopter = worker.adopter, handoff]() mutable
                {
                    adopter(handoff->take());
                });
        return true;
    }

    std::size_t ConnectionDistributor::distributedCount() const noexcept
    {
        return m_distributedCount;
    }
} // namespace AsynGyanis::Core
