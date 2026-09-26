// TcpStream 单元测试：首次读取真实收数据、缓冲消费顺序、分隔符截断推进与整块写入
#include "Net/Tcp/TcpStream.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/SystemException.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/AsyncSocket.h"
#include "Platform/IO/FileDescriptor.h"

#include "CoreTestSupport.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 一般等待上限：描述符对上的本机读写都在毫秒级完成；超时即判失败，绝不允许无界等待
        constexpr std::chrono::milliseconds kWaitTimeout{2000};

        /// 大报文（数千字节）写入与读取的等待上限，留足内核缓冲排空与线程调度的余量
        constexpr std::chrono::milliseconds kLargePayloadWaitTimeout{6000};

        /// 「断言某事不发生」时用的观察窗口：负向检查只需短窗口，不该为等待失败而耗满常规上限
        constexpr std::chrono::milliseconds kNegativeCheckTimeout{200};

        /// TcpStream 内部单次底层接收的容量：实现里是私有的 kReadBufferCapacity，
        /// 测试按同一口径复述一份数值，因为它本身就是对外的行为契约，改动必须让用例一起失败
        constexpr std::size_t kInternalReadBufferCapacity = 4096;

        /// 从对端描述符逐次读取时用的切片缓冲大小
        constexpr std::size_t kPeerChunkLength = 4096;

        /**
         * @brief 驱动协程结束时记录的失败类别
         */
        enum class FailureKind
        {
            None,            ///< 正常跑完，没有抛异常
            SystemException, ///< 抛出 Base::SystemException（底层 I/O 错误）
            BaseException,   ///< 抛出其它 Base::Exception（「读不满」一类协议层错误）
            UnknownException ///< 抛出框架外的异常
        };

        /**
         * @brief 一次协程驱动的公共结果槽
         *
         * @details completed 以 release 语义发布，其余字段只在 completed 为 true 之后可读：
         *          协程在事件循环线程上写、用例在主线程读，这一对 release/acquire 就是两者之间
         *          唯一的同步关系，因此不必再加互斥量。
         */
        struct OperationOutcome
        {
            std::atomic<bool> completed{false};           ///< 驱动协程是否已结束
            FailureKind       failure{FailureKind::None}; ///< 结束原因
            ssize_t           byteCount{-1};              ///< read/write 返回的字节数，未执行为 -1
            std::string       text;                       ///< readUntil 的返回值，或 read 拿到的字节序列
        };

        /**
         * @brief 在超时上限内逐毫秒轮询等待条件成立（定义见 CoreTestSupport.h，各调用点自带超时）
         */
        using AsynGyanis::Core::TestSupport::waitForCondition;

        /**
         * @brief 全双工描述符对夹具
         *
         * @details 一端交给被测 TcpStream（所有权随 AsyncSocket 转移，本对象不再关闭它），
         *          另一端由测试自己读写原始字节：既不占端口，又能精确控制分包与「对端关闭」的时机。
         * @note 析构只关闭仍归本夹具持有的那一端，避免重复关闭已被复用的描述符编号。
         */
        class LoopbackDescriptorPair
        {
        public:
            LoopbackDescriptorPair()
            {
                m_creationSucceeded = Platform::FileDescriptor::createPair(m_streamSide, m_peerSide);
                if (!m_creationSucceeded)
                {
                    m_streamSide = Platform::FileDescriptor::kInvalid;
                    m_peerSide   = Platform::FileDescriptor::kInvalid;
                }
            }

            ~LoopbackDescriptorPair()
            {
                Platform::FileDescriptor::close(m_streamSide);
                Platform::FileDescriptor::close(m_peerSide);
            }

            LoopbackDescriptorPair(const LoopbackDescriptorPair &)            = delete;
            LoopbackDescriptorPair &operator=(const LoopbackDescriptorPair &) = delete;

            /// 两端是否都创建成功
            [[nodiscard]] bool isValid() const noexcept
            {
                return m_creationSucceeded;
            }

            /// 测试自己那一端：用它写入被测流要读的数据、读取被测流写出的数据
            [[nodiscard]] int peerSide() const noexcept
            {
                return m_peerSide;
            }

            /**
             * @brief 取走交给被测流的一端，并放弃本夹具对它的关闭责任
             * @return int 描述符编号
             */
            int takeStreamSide() noexcept
            {
                return std::exchange(m_streamSide, Platform::FileDescriptor::kInvalid);
            }

            /// 关闭测试自己那一端，用来模拟「对端正常关闭连接」
            void closePeerSide() noexcept
            {
                Platform::FileDescriptor::close(m_peerSide);
                m_peerSide = Platform::FileDescriptor::kInvalid;
            }

        private:
            bool m_creationSucceeded{false};                       ///< 配对是否成功
            int  m_streamSide{Platform::FileDescriptor::kInvalid}; ///< 交给被测流的一端
            int  m_peerSide{Platform::FileDescriptor::kInvalid};   ///< 测试自己持有的一端
        };

        /**
         * @brief 在独立线程上驱动 EventLoop 的夹具（定义见 CoreTestSupport.h，借用模式）
         * @note 必须在协程任务对象之后构造：析构顺序保证「先 join 循环线程，再销毁协程帧」，
         *       否则循环线程可能恢复一个已被销毁的句柄
         */
        using AsynGyanis::Core::TestSupport::EventLoopThread;

        /**
         * @brief 把一段协程逻辑跑到结束，并按类型记录它抛出的异常
         * @param outcome 结果槽
         * @param operationFactory 产生被测协程的工厂
         * @return Core::Task<> 协程，结束时 outcome.completed 已置位
         */
        Core::Task<> runOnEventLoop(OperationOutcome &outcome, const std::function<Core::Task<>()> operationFactory)
        {
            try
            {
                co_await operationFactory();
            } catch (const Base::SystemException &)
            {
                outcome.failure = FailureKind::SystemException;
            } catch (const Base::Exception &)
            {
                outcome.failure = FailureKind::BaseException;
            } catch (...)
            {
                outcome.failure = FailureKind::UnknownException;
            }
            outcome.completed.store(true, std::memory_order_release);
            co_return;
        }

        /**
         * @brief 投递驱动协程并等待其结束
         * @return true 在时限内结束（false 表示用例应当立即失败，而不是继续等）
         */
        bool runAndAwaitCompletion(EventLoopThread &loopThread, Core::Task<> &driverTask, OperationOutcome &outcome, const std::chrono::milliseconds timeout)
        {
            loopThread.schedule(driverTask);
            return waitForCondition([&outcome] { return outcome.completed.load(std::memory_order_acquire); }, timeout);
        }

        /// 结果槽是否已在时限内完成且没有抛异常
        bool completedCleanly(const OperationOutcome &outcome)
        {
            return outcome.completed.load(std::memory_order_acquire) && outcome.failure == FailureKind::None;
        }

        /**
         * @brief 把整段字节写入对端描述符
         * @details 描述符对是非阻塞的，一次 write 可能短写或返回 EAGAIN，故按截止时间重试；
         *          超时返回 false 让用例干净失败，绝不把测试线程挂死。
         * @return true 全部字节已被内核接收
         */
        bool writeToPeerFully(const int descriptor, const std::string_view payload, const std::chrono::milliseconds timeout)
        {
            const auto  deadline      = std::chrono::steady_clock::now() + timeout;
            std::size_t writtenLength = 0;

            while (writtenLength < payload.size())
            {
                const ssize_t writeLength = Platform::FileDescriptor::write(descriptor, payload.data() + writtenLength, payload.size() - writtenLength);
                if (writeLength > 0)
                {
                    writtenLength += static_cast<std::size_t>(writeLength);
                    continue;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        }

        /**
         * @brief 轮询读取对端描述符，直到谓词满足或超时
         * @param descriptor 测试自己持有的那一端
         * @param received 输出：累计读到的字节
         * @param predicate 判定是否可以停止读取
         * @param timeout 等待上限
         * @return true 谓词在时限内成立
         */
        bool readFromPeerUntil(const int descriptor, std::string &received, const std::function<bool(const std::string &)> &predicate, const std::chrono::milliseconds timeout)
        {
            const auto                         deadline = std::chrono::steady_clock::now() + timeout;
            std::array<char, kPeerChunkLength> chunkStorage{};

            while (!predicate(received))
            {
                const ssize_t readLength = Platform::FileDescriptor::read(descriptor, chunkStorage.data(), chunkStorage.size());
                if (readLength > 0)
                {
                    received.append(chunkStorage.data(), static_cast<std::size_t>(readLength));
                    continue;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    break;
                }
                // 0 表示对端已关闭、-1 表示暂无数据或出错：都让出时间片后继续轮询，直到超时
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return predicate(received);
        }

        /**
         * @brief 生成一段每个位置都可辨识的文本，用来证明「交出的是第几段字节」
         * @details 用 "<序号>" 依次铺开，任意偏移处的内容都不同，截断/重复投递一眼可辨
         * @param length 需要的字节数
         * @return std::string 恰好 length 字节
         */
        std::string makeIndexedPayload(const std::size_t length)
        {
            std::string payload;
            payload.reserve(length);
            for (std::size_t sequenceNumber = 0; payload.size() < length; ++sequenceNumber)
            {
                payload.push_back('<');
                payload += std::to_string(sequenceNumber);
                payload.push_back('>');
            }
            payload.resize(length);
            return payload;
        }
    } // namespace

    TEST(TcpStream, FirstReadReturnsOnlyPayloadSentByPeer)
    {
        Core::EventLoop        loop;
        LoopbackDescriptorPair descriptors;
        ASSERT_TRUE(descriptors.isValid()) << "全双工描述符对创建失败，无法在不占端口的前提下验证首次读取";

        // 预分配缓冲区不等于已收到数据：对端只发了 5 个字节，返回长度必须是 5 而不是缓冲区容量，
        // 否则会把整块未初始化字节当成有效数据交给调用方
        ASSERT_TRUE(writeToPeerFully(descriptors.peerSide(), "HELLO", kWaitTimeout));

        TcpStream                                     stream(Core::AsyncSocket(loop, descriptors.takeStreamSide()));
        OperationOutcome                              outcome;
        std::array<char, kInternalReadBufferCapacity> receiveStorage{};
        Core::Task<>                                  driverTask = runOnEventLoop(outcome,
                                                                                  [&stream, &outcome, &receiveStorage]() -> Core::Task<>
                                                                                  {
                                                     const ssize_t readLength = co_await stream.read(receiveStorage.data(), receiveStorage.size());
                                                     outcome.byteCount        = readLength;
                                                     if (readLength > 0)
                                                     {
                                                         outcome.text.assign(receiveStorage.data(), static_cast<std::size_t>(readLength));
                                                     }
                                                     co_return;
                                                                                  });

        EventLoopThread loopThread(loop);
        ASSERT_TRUE(runAndAwaitCompletion(loopThread, driverTask, outcome, kWaitTimeout)) << "首次读取未在时限内完成：疑似把未初始化缓冲当成数据后又挂住";
        EXPECT_EQ(outcome.failure, FailureKind::None);
        EXPECT_EQ(outcome.byteCount, 5);
        EXPECT_EQ(outcome.text, "HELLO");
    }

    TEST(TcpStream, ReadClampsToInternalBufferCapacityAndKeepsRemainder)
    {
        Core::EventLoop        loop;
        LoopbackDescriptorPair descriptors;
        ASSERT_TRUE(descriptors.isValid());

        // 单次底层接收的上限就是内部缓冲区容量：多出的字节留在内核里给下一次读取，
        // 既不能凭空丢掉，也不能一次硬塞给调用方
        const std::string payload = makeIndexedPayload(kInternalReadBufferCapacity + 904);
        ASSERT_TRUE(writeToPeerFully(descriptors.peerSide(), payload, kLargePayloadWaitTimeout));

        TcpStream                                         stream(Core::AsyncSocket(loop, descriptors.takeStreamSide()));
        OperationOutcome                                  firstOutcome;
        OperationOutcome                                  secondOutcome;
        std::array<char, kInternalReadBufferCapacity + 1> firstStorage{};
        std::array<char, 2048>                            secondStorage{};

        Core::Task<> driverTask = runOnEventLoop(firstOutcome,
                                                 [&stream, &firstOutcome, &secondOutcome, &firstStorage, &secondStorage]() -> Core::Task<>
                                                 {
                                                     const ssize_t firstLength = co_await stream.read(firstStorage.data(), firstStorage.size());
                                                     firstOutcome.byteCount    = firstLength;
                                                     if (firstLength > 0)
                                                     {
                                                         firstOutcome.text.assign(firstStorage.data(), static_cast<std::size_t>(firstLength));
                                                     }
                                                     firstOutcome.completed.store(true, std::memory_order_release);

                                                     const ssize_t secondLength = co_await stream.read(secondStorage.data(), secondStorage.size());
                                                     secondOutcome.byteCount    = secondLength;
                                                     if (secondLength > 0)
                                                     {
                                                         secondOutcome.text.assign(secondStorage.data(), static_cast<std::size_t>(secondLength));
                                                     }
                                                     secondOutcome.completed.store(true, std::memory_order_release);
                                                     co_return;
                                                 });

        EventLoopThread loopThread(loop);
        ASSERT_TRUE(runAndAwaitCompletion(loopThread, driverTask, firstOutcome, kLargePayloadWaitTimeout));
        EXPECT_EQ(firstOutcome.byteCount, static_cast<ssize_t>(kInternalReadBufferCapacity));
        ASSERT_TRUE(waitForCondition([&secondOutcome] { return secondOutcome.completed.load(std::memory_order_acquire); }, kLargePayloadWaitTimeout))
                << "续读超时：等待上界为 kLargePayloadWaitTimeout，属依赖时序的一步";
        EXPECT_EQ(secondOutcome.byteCount, 904);
        EXPECT_EQ(firstOutcome.text + secondOutcome.text, payload);
    }

    TEST(TcpStream, ReadConsumesBufferedBytesInOrderBeforeReadingAgain)
    {
        Core::EventLoop        loop;
        LoopbackDescriptorPair descriptors;
        ASSERT_TRUE(descriptors.isValid());

        ASSERT_TRUE(writeToPeerFully(descriptors.peerSide(), "abcdefghij", kWaitTimeout));
        // 十个字节已进内核接收缓冲；随后对端关闭，最后一次读才能拿 EOF 当「缓冲区刚好被吃空」的证据
        descriptors.closePeerSide();

        TcpStream        stream(Core::AsyncSocket(loop, descriptors.takeStreamSide()));
        OperationOutcome outcome;
        Core::Task<>     driverTask = runOnEventLoop(outcome,
                                                     [&stream, &outcome]() -> Core::Task<>
                                                     {
                                                     std::array<char, 4> sliceStorage{};
                                                     std::string         concatenated;

                                                     // 三次各要 4 字节：10 个字节全在内部缓冲区里，第 2、3 次不该再碰套接字
                                                     for (std::size_t round = 0; round < 3; ++round)
                                                     {
                                                         const ssize_t readLength = co_await stream.read(sliceStorage.data(), sliceStorage.size());
                                                         if (readLength > 0)
                                                         {
                                                             concatenated.append(sliceStorage.data(), static_cast<std::size_t>(readLength));
                                                         }
                                                     }
                                                     outcome.text = concatenated;
                                                     outcome.completed.store(true, std::memory_order_release);

                                                     // 缓冲区此刻恰好空了：下一次读必须走到套接字，并因为对端关闭返回 0
                                                     outcome.byteCount = co_await stream.read(sliceStorage.data(), sliceStorage.size());
                                                     co_return;
                                                     });

        EventLoopThread loopThread(loop);
        ASSERT_TRUE(runAndAwaitCompletion(loopThread, driverTask, outcome, kWaitTimeout));
        EXPECT_EQ(outcome.text, "abcdefghij");
        EXPECT_EQ(outcome.byteCount, 0);
    }

    TEST(TcpStream, ReadWithZeroLengthReturnsZeroWithoutBlocking)
    {
        Core::EventLoop        loop;
        LoopbackDescriptorPair descriptors;
        ASSERT_TRUE(descriptors.isValid());

        // 对端一个字节都没发：零长度读取必须立即返回，而不是白挂起一次协程
        TcpStream           stream(Core::AsyncSocket(loop, descriptors.takeStreamSide()));
        OperationOutcome    outcome;
        std::array<char, 8> receiveStorage{};
        Core::Task<>        driverTask = runOnEventLoop(outcome,
                                                        [&stream, &outcome, &receiveStorage]() -> Core::Task<>
                                                        {
                                                     outcome.byteCount = co_await stream.read(receiveStorage.data(), 0);
                                                     co_return;
                                                        });

        EventLoopThread loopThread(loop);
        ASSERT_TRUE(runAndAwaitCompletion(loopThread, driverTask, outcome, kWaitTimeout)) << "零长度读取挂住了：等待上界 kWaitTimeout";
        EXPECT_EQ(outcome.byteCount, 0);
        EXPECT_EQ(outcome.failure, FailureKind::None);
    }

    TEST(TcpStream, ReadReturnsZeroWhenPeerClosedWithoutBufferedBytes)
    {
        Core::EventLoop        loop;
        LoopbackDescriptorPair descriptors;
        ASSERT_TRUE(descriptors.isValid());

        // 先关对端再读：0 是「对端正常关闭」的唯一信号，不能被当成读错误
        descriptors.closePeerSide();

        TcpStream            stream(Core::AsyncSocket(loop, descriptors.takeStreamSide()));
        OperationOutcome     outcome;
        std::array<char, 32> receiveStorage{};
        Core::Task<>         driverTask = runOnEventLoop(outcome,
                                                         [&stream, &outcome, &receiveStorage]() -> Core::Task<>
                                                         {
                                                     outcome.byteCount = co_await stream.read(receiveStorage.data(), receiveStorage.size());
                                                     co_return;
                                                         });

        EventLoopThread loopThread(loop);
        ASSERT_TRUE(runAndAwaitCompletion(loopThread, driverTask, outcome, kWaitTimeout));
        EXPECT_EQ(outcome.byteCount, 0);
        EXPECT_EQ(outcome.failure, FailureKind::None);
    }

    TEST(TcpStream, ReadExactCompletesWhenAllRequestedBytesAreAvailable)
    {
        Core::EventLoop        loop;
        LoopbackDescriptorPair descriptors;
        ASSERT_TRUE(descriptors.isValid());

        ASSERT_TRUE(writeToPeerFully(descriptors.peerSide(), "abcdefgh", kWaitTimeout));

        TcpStream           stream(Core::AsyncSocket(loop, descriptors.takeStreamSide()));
        OperationOutcome    outcome;
        std::array<char, 8> receiveStorage{};
        Core::Task<>        driverTask = runOnEventLoop(outcome,
                                                        [&stream, &outcome, &receiveStorage]() -> Core::Task<>
                                                        {
                                                     co_await stream.readExact(receiveStorage.data(), receiveStorage.size());
                                                     outcome.text.assign(receiveStorage.data(), receiveStorage.size());
                                                     outcome.byteCount = static_cast<ssize_t>(receiveStorage.size());
                                                     co_return;
                                                        });

        EventLoopThread loopThread(loop);
        ASSERT_TRUE(runAndAwaitCompletion(loopThread, driverTask, outcome, kWaitTimeout));
        EXPECT_TRUE(completedCleanly(outcome));
        EXPECT_EQ(outcome.text, "abcdefgh");
    }

    TEST(TcpStream, ReadExactThrowsWhenPeerClosesBeforeLengthIsSatisfied)
    {
        Core::EventLoop        loop;
        LoopbackDescriptorPair descriptors;
        ASSERT_TRUE(descriptors.isValid());

        // 短读属于「不可能再凑满」：上抛框架异常，而不是把半截缓冲区交给调用方
        ASSERT_TRUE(writeToPeerFully(descriptors.peerSide(), "abc", kWaitTimeout));
        descriptors.closePeerSide();

        TcpStream           stream(Core::AsyncSocket(loop, descriptors.takeStreamSide()));
        OperationOutcome    outcome;
        std::array<char, 8> receiveStorage{};
        Core::Task<>        driverTask = runOnEventLoop(outcome,
                                                        [&stream, &outcome, &receiveStorage]() -> Core::Task<>
                                                        {
                                                     co_await stream.readExact(receiveStorage.data(), receiveStorage.size());
                                                     outcome.text.assign(receiveStorage.data(), receiveStorage.size());
                                                     co_return;
                                                        });

        EventLoopThread loopThread(loop);
        ASSERT_TRUE(runAndAwaitCompletion(loopThread, driverTask, outcome, kWaitTimeout));
        EXPECT_EQ(outcome.failure, FailureKind::BaseException);
        EXPECT_TRUE(outcome.text.empty());
    }

    TEST(TcpStream, ReadUntilReturnsTextBeforeDelimiterAndSkipsIt)
    {
        Core::EventLoop        loop;
        LoopbackDescriptorPair descriptors;
        ASSERT_TRUE(descriptors.isValid());

        ASSERT_TRUE(writeToPeerFully(descriptors.peerSide(), "one\ntwo\n", kWaitTimeout));

        TcpStream        stream(Core::AsyncSocket(loop, descriptors.takeStreamSide()));
        OperationOutcome firstOutcome;
        OperationOutcome secondOutcome;
        Core::Task<>     driverTask = runOnEventLoop(firstOutcome,
                                                     [&stream, &firstOutcome, &secondOutcome]() -> Core::Task<>
                                                     {
                                                     firstOutcome.text = co_await stream.readUntil('\n');
                                                     firstOutcome.completed.store(true, std::memory_order_release);

                                                     secondOutcome.text = co_await stream.readUntil('\n');
                                                     secondOutcome.completed.store(true, std::memory_order_release);
                                                     co_return;
                                                     });

        EventLoopThread loopThread(loop);
        ASSERT_TRUE(runAndAwaitCompletion(loopThread, driverTask, firstOutcome, kWaitTimeout));
        EXPECT_EQ(firstOutcome.text, "one");
        ASSERT_TRUE(waitForCondition([&secondOutcome] { return secondOutcome.completed.load(std::memory_order_acquire); }, kWaitTimeout));
        // 第二次必须从分隔符之后开始：结果里再出现 "one" 就说明分隔符没被就地消费
        EXPECT_EQ(secondOutcome.text, "two");
    }

    TEST(TcpStream, ReadUntilTruncationAdvancesConsumedPosition)
    {
        Core::EventLoop        loop;
        LoopbackDescriptorPair descriptors;
        ASSERT_TRUE(descriptors.isValid());

        // 回归防护：readUntil 触顶截断时必须推进消费位置，否则这段字节会被下一次读取重复交出。
        // 载荷 "0123456789\nabcdefgh\n"、maximumSize = 5 时结果可逐字节推算：
        //   第 1 次截在 "01234"（分隔符仍在缓冲区里）
        //   第 2 次必须接着给 "56789" 并吃掉分隔符，而不是重发 "01234"
        //   第 3 次再截在 "abcde"
        ASSERT_TRUE(writeToPeerFully(descriptors.peerSide(), "0123456789\nabcdefgh\n", kWaitTimeout));

        TcpStream        stream(Core::AsyncSocket(loop, descriptors.takeStreamSide()));
        OperationOutcome firstOutcome;
        OperationOutcome secondOutcome;
        OperationOutcome thirdOutcome;
        Core::Task<>     driverTask = runOnEventLoop(firstOutcome,
                                                     [&stream, &firstOutcome, &secondOutcome, &thirdOutcome]() -> Core::Task<>
                                                     {
                                                     constexpr std::size_t kTruncateSize = 5;
                                                     firstOutcome.text                   = co_await stream.readUntil('\n', kTruncateSize);
                                                     firstOutcome.completed.store(true, std::memory_order_release);

                                                     secondOutcome.text = co_await stream.readUntil('\n', kTruncateSize);
                                                     secondOutcome.completed.store(true, std::memory_order_release);

                                                     thirdOutcome.text = co_await stream.readUntil('\n', kTruncateSize);
                                                     thirdOutcome.completed.store(true, std::memory_order_release);
                                                     co_return;
                                                     });

        EventLoopThread loopThread(loop);
        ASSERT_TRUE(runAndAwaitCompletion(loopThread, driverTask, firstOutcome, kWaitTimeout));
        EXPECT_EQ(firstOutcome.text, "01234");
        ASSERT_TRUE(waitForCondition([&thirdOutcome] { return thirdOutcome.completed.load(std::memory_order_acquire); }, kWaitTimeout));
        EXPECT_EQ(secondOutcome.text, "56789");
        EXPECT_EQ(thirdOutcome.text, "abcde");
    }

    TEST(TcpStream, ReadUntilReturnsPrefixWhenPeerClosesWithoutDelimiter)
    {
        Core::EventLoop        loop;
        LoopbackDescriptorPair descriptors;
        ASSERT_TRUE(descriptors.isValid());

        // maximumSize 传 0 表示不限长度；对端在遇到分隔符前关闭时按「读到哪算哪」返回前缀
        ASSERT_TRUE(writeToPeerFully(descriptors.peerSide(), "abc", kWaitTimeout));
        descriptors.closePeerSide();

        TcpStream        stream(Core::AsyncSocket(loop, descriptors.takeStreamSide()));
        OperationOutcome outcome;
        Core::Task<>     driverTask = runOnEventLoop(outcome,
                                                     [&stream, &outcome]() -> Core::Task<>
                                                     {
                                                     outcome.text = co_await stream.readUntil('\n', 0);
                                                     co_return;
                                                     });

        EventLoopThread loopThread(loop);
        ASSERT_TRUE(runAndAwaitCompletion(loopThread, driverTask, outcome, kWaitTimeout));
        EXPECT_EQ(outcome.text, "abc");
        EXPECT_EQ(outcome.failure, FailureKind::None);
    }

    TEST(TcpStream, ReadUntilStopsAtRequestedMaximumSizeWithoutRedelivering)
    {
        Core::EventLoop        loop;
        LoopbackDescriptorPair descriptors;
        ASSERT_TRUE(descriptors.isValid());

        const std::string payload = makeIndexedPayload(100);
        ASSERT_TRUE(writeToPeerFully(descriptors.peerSide(), payload, kWaitTimeout));

        // 流里没有分隔符时 maximumSize 就是硬边界：只交出一半，剩下的一半留给下一次读取
        TcpStream        stream(Core::AsyncSocket(loop, descriptors.takeStreamSide()));
        OperationOutcome firstOutcome;
        OperationOutcome secondOutcome;
        Core::Task<>     driverTask = runOnEventLoop(firstOutcome,
                                                     [&stream, &firstOutcome, &secondOutcome]() -> Core::Task<>
                                                     {
                                                     constexpr std::size_t kRequestedMaximumSize = 40;
                                                     firstOutcome.text                           = co_await stream.readUntil('\n', kRequestedMaximumSize);
                                                     firstOutcome.completed.store(true, std::memory_order_release);

                                                     secondOutcome.text = co_await stream.readUntil('\n', kRequestedMaximumSize);
                                                     secondOutcome.completed.store(true, std::memory_order_release);
                                                     co_return;
                                                     });

        EventLoopThread loopThread(loop);
        ASSERT_TRUE(runAndAwaitCompletion(loopThread, driverTask, firstOutcome, kWaitTimeout));
        EXPECT_EQ(firstOutcome.text, payload.substr(0, 40));
        ASSERT_TRUE(waitForCondition([&secondOutcome] { return secondOutcome.completed.load(std::memory_order_acquire); }, kWaitTimeout));
        EXPECT_EQ(secondOutcome.text, payload.substr(40, 40));
    }

    TEST(TcpStream, WriteZeroLengthDoesNotSendAnything)
    {
        Core::EventLoop        loop;
        LoopbackDescriptorPair descriptors;
        ASSERT_TRUE(descriptors.isValid());

        TcpStream        stream(Core::AsyncSocket(loop, descriptors.takeStreamSide()));
        OperationOutcome outcome;
        Core::Task<>     driverTask = runOnEventLoop(outcome,
                                                     [&stream, &outcome]() -> Core::Task<>
                                                     {
                                                     outcome.byteCount = co_await stream.write("x", 0);
                                                     co_return;
                                                     });

        EventLoopThread loopThread(loop);
        ASSERT_TRUE(runAndAwaitCompletion(loopThread, driverTask, outcome, kWaitTimeout));
        EXPECT_EQ(outcome.byteCount, 0);

        // 反向验证：对端在 kNegativeCheckTimeout（200ms）这个上界内一个字节都不该收到
        std::string received;
        EXPECT_FALSE(readFromPeerUntil(descriptors.peerSide(), received, [](const std::string &accumulated) { return !accumulated.empty(); }, kNegativeCheckTimeout));
    }

    TEST(TcpStream, WriteAllSendsWholePayloadToPeer)
    {
        Core::EventLoop        loop;
        LoopbackDescriptorPair descriptors;
        ASSERT_TRUE(descriptors.isValid());

        const std::string payload = makeIndexedPayload(3000);
        TcpStream         stream(Core::AsyncSocket(loop, descriptors.takeStreamSide()));
        OperationOutcome  outcome;
        Core::Task<>      driverTask = runOnEventLoop(outcome,
                                                      [&stream, &outcome, &payload]() -> Core::Task<>
                                                      {
                                                     co_await stream.writeAll(payload.data(), payload.size());
                                                     outcome.byteCount = static_cast<ssize_t>(payload.size());
                                                     co_return;
                                                      });

        EventLoopThread loopThread(loop);
        ASSERT_TRUE(runAndAwaitCompletion(loopThread, driverTask, outcome, kLargePayloadWaitTimeout)) << "写入 3000 字节超时：上界 kLargePayloadWaitTimeout";
        EXPECT_TRUE(completedCleanly(outcome));

        std::string received;
        ASSERT_TRUE(readFromPeerUntil(
                descriptors.peerSide(), received, [&](const std::string &accumulated) { return accumulated.size() >= payload.size(); }, kLargePayloadWaitTimeout));
        EXPECT_EQ(received, payload);
    }

    TEST(TcpStream, CloseDiscardsBufferedDataAndRejectsFurtherReads)
    {
        Core::EventLoop        loop;
        LoopbackDescriptorPair descriptors;
        ASSERT_TRUE(descriptors.isValid());

        const int streamSideDescriptor = descriptors.takeStreamSide();
        ASSERT_GE(streamSideDescriptor, 0);

        TcpStream stream(Core::AsyncSocket(loop, streamSideDescriptor));
        EXPECT_EQ(stream.socket().fileDescriptor(), streamSideDescriptor);

        stream.close();
        EXPECT_EQ(stream.socket().fileDescriptor(), Platform::FileDescriptor::kInvalid);

        // 描述符已失效：再读必须立刻以系统错误失败，而不是静默返回旧缓冲区里的残留字节
        OperationOutcome     outcome;
        std::array<char, 16> receiveStorage{};
        Core::Task<>         driverTask = runOnEventLoop(outcome,
                                                         [&stream, &outcome, &receiveStorage]() -> Core::Task<>
                                                         {
                                                     outcome.byteCount = co_await stream.read(receiveStorage.data(), receiveStorage.size());
                                                     co_return;
                                                         });

        EventLoopThread loopThread(loop);
        ASSERT_TRUE(runAndAwaitCompletion(loopThread, driverTask, outcome, kWaitTimeout));
        EXPECT_EQ(outcome.failure, FailureKind::SystemException);
    }

    TEST(TcpStream, MoveConstructionTransfersSocketAndBufferedBytes)
    {
        Core::EventLoop        loop;
        LoopbackDescriptorPair descriptors;
        ASSERT_TRUE(descriptors.isValid());

        ASSERT_TRUE(writeToPeerFully(descriptors.peerSide(), "hello", kWaitTimeout));

        TcpStream        sourceStream(Core::AsyncSocket(loop, descriptors.takeStreamSide()));
        OperationOutcome outcome;
        Core::Task<>     driverTask = runOnEventLoop(outcome,
                                                     [&sourceStream, &outcome]() -> Core::Task<>
                                                     {
                                                     std::array<char, 2> prefixStorage{};
                                                     outcome.byteCount = co_await sourceStream.read(prefixStorage.data(), prefixStorage.size());

                                                     // 移动后：描述符所有权与「尚未派发的缓冲内容」都跟着走，续读既不能丢字节也不能重复投递
                                                     TcpStream           movedStream(std::move(sourceStream));
                                                     std::array<char, 8> remainderStorage{};
                                                     const ssize_t       remainderLength = co_await movedStream.read(remainderStorage.data(), remainderStorage.size());
                                                     if (remainderLength > 0)
                                                     {
                                                         outcome.text.assign(remainderStorage.data(), static_cast<std::size_t>(remainderLength));
                                                     }
                                                     co_return;
                                                     });

        EventLoopThread loopThread(loop);
        ASSERT_TRUE(runAndAwaitCompletion(loopThread, driverTask, outcome, kWaitTimeout));
        EXPECT_EQ(outcome.byteCount, 2);
        EXPECT_EQ(outcome.text, "llo");
    }

    TEST(TcpStream, ClassIsMovableButNotCopyable)
    {
        // 值语义契约：接管描述符所有权的流可以搬走，但绝不允许复制出第二个持有同一描述符的对象
        static_assert(!std::is_default_constructible_v<TcpStream>, "TcpStream 不应能被默认构造：它必须接管一个套接字");
        static_assert(!std::is_copy_constructible_v<TcpStream>, "TcpStream 禁止拷贝构造");
        static_assert(!std::is_copy_assignable_v<TcpStream>, "TcpStream 禁止拷贝赋值");
        static_assert(std::is_move_constructible_v<TcpStream>, "TcpStream 应可移动构造");
        static_assert(std::is_move_assignable_v<TcpStream>, "TcpStream 应可移动赋值");
        static_assert(std::is_nothrow_move_constructible_v<TcpStream>, "移动构造应 noexcept，便于放进容器");
        SUCCEED() << "以上均为编译期断言";
    }

    TEST(TcpStream, DefaultMaximumReadSizeBoundsMalformedTraffic)
    {
        // 64 KiB 是 readUntil 的默认档位：容得下正常协议头部，同时给畸形流量一个明确内存上界
        static_assert(kDefaultMaximumReadSize == 65536, "readUntil 默认上限的改动必须同步文档与用例");
        EXPECT_EQ(kDefaultMaximumReadSize, static_cast<std::size_t>(65536));
    }
} // namespace AsynGyanis::Net
