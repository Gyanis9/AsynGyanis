// 零拷贝发送的单元测试：整段字节一致、源文件末尾语义，以及「慢消费者不得阻塞调用方」 Windows 下本文件整体展开为空：该平台没有可用的零拷贝发送原语，Platform 层因此不声明
// sendFileChunk，原因（实测的阻塞与进度不可反推）见 Socket.h 中该函数的说明
#include "Platform/Platform.h"
#include "PlatformTestSupport.h"

// 平台判定要先有 Platform.h：下面的整体护栏靠它给出的 ASYN_PLATFORM_WIN32
#if !ASYN_PLATFORM_WIN32

#include "Platform/IO/Socket.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/MemoryMappedFile.h"
#include "Platform/System/PlatformError.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace AsynGyanis::Platform
{
    namespace
    {
        using TestSupport::TemporaryDirectory;

        /// 有界重试统一使用的最长等待毫秒数，避免任何一步无限阻塞
        constexpr int kWaitTimeoutMilliseconds = 5000;

        /// 整段发送用例的文件字节数：足够跨多次零拷贝分块
        constexpr std::size_t kTransferFileBytes = 256 * 1024;

        /// 源文件句柄：与 sendFileChunk 的形参一致（Linux 上是文件描述符）
        using NativeFileHandle = int;

        /**
         * @brief 取映射文件持有的本地句柄
         * @param mappedFile 已打开的映射文件
         * @return NativeFileHandle 文件描述符
         */
        NativeFileHandle nativeHandleFor(const MemoryMappedFile &mappedFile)
        {
            return mappedFile.nativeFileDescriptor();
        }

        /**
         * @brief 以二进制方式写一个文件
         * @details 不用 TestSupport::TemporaryDirectory::writeFile：它按文本模式打开，会把正文里的
         *          0x0A 折行，落盘字节与用例给出的字节不再一致（伪随机正文里每 256 字节就有一个）
         * @param directory 目标目录
         * @param fileName 文件名
         * @param content 文件内容（二进制）
         * @return true 写入成功
         */
        bool writeBinaryFile(const std::filesystem::path &directory, const std::string &fileName, const std::string &content)
        {
            std::ofstream file(directory / fileName, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!file.is_open())
            {
                return false;
            }
            file.write(content.data(), static_cast<std::streamsize>(content.size()));
            return file.good();
        }

        /**
         * @brief 造一段确定性的伪随机字节
         * @details 不用重复填充：重复内容会让「少发一段、错位」这类缺陷在比对里看上去仍然相等
         * @param byteCount 字节数
         * @return std::string 二进制内容
         */
        std::string makeDeterministicBytes(const std::size_t byteCount)
        {
            std::string   bytes;
            std::uint32_t state = 0x5eed1234u;
            bytes.reserve(byteCount);
            for (std::size_t index = 0; index < byteCount; ++index)
            {
                state = state * 1664525u + 1013904223u;
                bytes.push_back(static_cast<char>((state >> 16) & 0xffu));
            }
            return bytes;
        }

        /**
         * @brief 一条回环连接的两端：服务端（发送方）与客户端（接收方），都已置为非阻塞
         */
        struct LoopbackPair
        {
            int server{FileDescriptor::kInvalid};   ///< 服务端描述符（接受自监听套接字）
            int client{FileDescriptor::kInvalid};   ///< 客户端描述符
            int listener{FileDescriptor::kInvalid}; ///< 监听描述符，用例结束时一并关闭

            ~LoopbackPair()
            {
                closeAll();
            }

            LoopbackPair() = default;

            // 移动必须把源侧的描述符置回无效：本类在析构里关闭句柄，若照搬默认的逐成员移动，
            // 源对象析构时会把同一批描述符再关一遍（可能关掉期间被复用的新描述符）
            LoopbackPair(LoopbackPair &&other) noexcept :
                server(std::exchange(other.server, FileDescriptor::kInvalid)),
                client(std::exchange(other.client, FileDescriptor::kInvalid)),
                listener(std::exchange(other.listener, FileDescriptor::kInvalid))
            {
            }

            LoopbackPair &operator=(LoopbackPair &&other) noexcept
            {
                if (this != &other)
                {
                    closeAll();
                    server   = std::exchange(other.server, FileDescriptor::kInvalid);
                    client   = std::exchange(other.client, FileDescriptor::kInvalid);
                    listener = std::exchange(other.listener, FileDescriptor::kInvalid);
                }
                return *this;
            }

            LoopbackPair(const LoopbackPair &) = delete;

            LoopbackPair &operator=(const LoopbackPair &) = delete;

            [[nodiscard]] bool isValid() const noexcept
            {
                return FileDescriptor::isValid(server) && FileDescriptor::isValid(client);
            }

        private:
            /// 关闭两端与监听描述符（幂等）
            void closeAll() noexcept
            {
                if (FileDescriptor::isValid(server))
                {
                    FileDescriptor::close(server);
                    server = FileDescriptor::kInvalid;
                }
                if (FileDescriptor::isValid(client))
                {
                    FileDescriptor::close(client);
                    client = FileDescriptor::kInvalid;
                }
                if (FileDescriptor::isValid(listener))
                {
                    FileDescriptor::close(listener);
                    listener = FileDescriptor::kInvalid;
                }
            }
        };

        /**
         * @brief 建立一条回环 TCP 连接，两端都转为非阻塞
         * @return LoopbackPair 两端描述符；任一步失败时两端均为无效
         */
        LoopbackPair makeLoopbackPair()
        {
            LoopbackPair pair;

            Socket::initialize();

            pair.listener = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (!FileDescriptor::isValid(pair.listener))
            {
                return pair;
            }

            sockaddr_in address{};
            address.sin_family      = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port        = 0;
            if (::bind(pair.listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
            {
                return pair;
            }

            socklen_t addressLength = sizeof(address);
            if (::getsockname(pair.listener, reinterpret_cast<sockaddr *>(&address), &addressLength) < 0)
            {
                return pair;
            }
            const std::uint16_t port = ntohs(address.sin_port);

            if (::listen(pair.listener, 1) < 0)
            {
                return pair;
            }

            const int client = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (!FileDescriptor::isValid(client))
            {
                return pair;
            }

            sockaddr_in serverAddress{};
            serverAddress.sin_family      = AF_INET;
            serverAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            serverAddress.sin_port        = htons(port);
            if (::connect(client, reinterpret_cast<sockaddr *>(&serverAddress), sizeof(serverAddress)) < 0)
            {
                FileDescriptor::close(client);
                return pair;
            }

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitTimeoutMilliseconds);
            while (std::chrono::steady_clock::now() < deadline)
            {
                const int accepted = Socket::accept(pair.listener, nullptr, nullptr);
                if (FileDescriptor::isValid(accepted))
                {
                    pair.server = accepted;
                    pair.client = client;
                    // 两端都必须非阻塞：零拷贝发送的契约就是「发不下就立刻返回」，阻塞套接字上
                    // 内核会一直等到发完，测出来的就不是本层的语义
                    FileDescriptor::setNonBlocking(pair.server);
                    FileDescriptor::setNonBlocking(pair.client);
                    return pair;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            FileDescriptor::close(client);
            return pair;
        }

        /**
         * @brief 把发送缓冲区压到最小值，让「缓冲区满」在很小的正文上就能出现
         * @param descriptor 目标描述符
         * @return true 设置成功
         */
        bool shrinkSendBuffer(const int descriptor)
        {
            constexpr int kTinySendBufferBytes = 4096;
            return ::setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &kTinySendBufferBytes, sizeof(kTinySendBufferBytes)) == 0;
        }

        /**
         * @brief 在给定上限内从描述符收字节，累积到 out
         * @param descriptor 接收描述符（非阻塞）
         * @param out 累积接收缓冲区
         * @param expectedByteCount 期望收到的字节数
         * @param timeoutMilliseconds 最长等待毫秒数
         * @return true 收满 expectedByteCount 个字节
         */
        bool receiveExactly(const int descriptor, std::string &out, const std::size_t expectedByteCount, const int timeoutMilliseconds)
        {
            char       buffer[16 * 1024];
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
            while (out.size() < expectedByteCount && std::chrono::steady_clock::now() < deadline)
            {
                const ssize_t received = FileDescriptor::read(descriptor, buffer, sizeof(buffer));
                if (received > 0)
                {
                    out.append(buffer, static_cast<std::size_t>(received));
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return out.size() >= expectedByteCount;
        }

        /**
         * @brief 逐字节比对，失败时报出首个差异位置（整段 EXPECT_EQ 会把二进制糊一页）
         * @param actual 实际字节
         * @param expected 期望字节
         */
        void expectBytesEqual(const std::string_view actual, const std::string_view expected)
        {
            ASSERT_EQ(actual.size(), expected.size());
            for (std::size_t index = 0; index < actual.size(); ++index)
            {
                if (actual[index] != expected[index])
                {
                    FAIL() << "正文在偏移 " << index << " 处不一致";
                }
            }
        }

        /**
         * @brief 把文件的字节零拷贝发给对端，一次调用发不完就按新偏移续发
         * @param socketDescriptor 目标套接字（非阻塞）
         * @param nativeHandle 源文件句柄
         * @param totalByteCount 期望发出的总字节数
         * @param errorCode 输出参数：非可重试错误的错误码（未出错时为 0）
         * @return std::size_t 实际发出的字节数；返回 0 表示源侧已到文件末尾
         */
        std::size_t sendWholeFile(const int socketDescriptor, const NativeFileHandle nativeHandle, const std::size_t totalByteCount,
                                  int &errorCode)
        {
            std::size_t sentByteCount = 0;
            errorCode                 = 0;
            const auto deadline       = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitTimeoutMilliseconds);
            while (sentByteCount < totalByteCount && std::chrono::steady_clock::now() < deadline)
            {
                const ssize_t chunk = Socket::sendFileChunk(socketDescriptor, nativeHandle, sentByteCount, totalByteCount - sentByteCount);
                if (chunk > 0)
                {
                    sentByteCount += static_cast<std::size_t>(chunk);
                    continue;
                }
                if (chunk == 0)
                {
                    break;
                }
                if (PlatformError::lastSocketErrorCode() == PlatformError::kWouldBlock)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                errorCode = PlatformError::lastSocketErrorCode();
                break;
            }
            return sentByteCount;
        }
    } // namespace

    /**
     * @brief 零拷贝发送把文件字节原样送到对端（两端平台的零拷贝原语都要过这条）
     */
    TEST(SocketZeroCopySend, TransfersFileBytesExactly)
    {
        const TemporaryDirectory temporaryDirectory("ZeroCopySend");
        const std::string        content  = makeDeterministicBytes(kTransferFileBytes);
        const std::string        filePath = (temporaryDirectory.path() / "payload.bin").string();
        ASSERT_TRUE(writeBinaryFile(temporaryDirectory.path(), "payload.bin", content));

        const MemoryMappedFile mappedFile = MemoryMappedFile::open(filePath);
        ASSERT_TRUE(mappedFile.isValid()) << "源文件映射失败";

        LoopbackPair pair = makeLoopbackPair();
        ASSERT_TRUE(pair.isValid()) << "回环连接建立失败";

        // 收与发分在两个线程：发送方续发时可能在等对端腾出缓冲，同一线程里没法既发又收
        std::string receivedBytes;
        const int   clientDescriptor = pair.client;
        std::thread receiver([clientDescriptor, &receivedBytes]
        {
            receiveExactly(clientDescriptor, receivedBytes, kTransferFileBytes, kWaitTimeoutMilliseconds);
        });

        int               sendErrorCode = 0;
        const std::size_t sentByteCount = sendWholeFile(pair.server, nativeHandleFor(mappedFile), kTransferFileBytes, sendErrorCode);
        receiver.join();

        EXPECT_EQ(sendErrorCode, 0) << "零拷贝发送报错：" << PlatformError::message(sendErrorCode);
        EXPECT_EQ(sentByteCount, kTransferFileBytes) << "零拷贝没有把整份文件发完";
        expectBytesEqual(receivedBytes, content);
    }

    /**
     * @brief 源文件已到末尾时返回 0（而不是报错、也不是按「可重试」吊住调用方）
     * @details 本用例把「发起长度超过文件剩余字节」这条路径钉死：调用方按返回值推进偏移，
     *          若末尾被折成 kWouldBlock，续发循环会原地空转成死循环
     */
    TEST(SocketZeroCopySend, ReportsEndOfFileWhenRequestExceedsFileLength)
    {
        constexpr std::size_t kFileBytes          = 64 * 1024;
        constexpr std::size_t kRequestedByteCount = 2 * kFileBytes;

        const TemporaryDirectory temporaryDirectory("ZeroCopySendEof");
        const std::string        content  = makeDeterministicBytes(kFileBytes);
        const std::string        filePath = (temporaryDirectory.path() / "short.bin").string();
        ASSERT_TRUE(writeBinaryFile(temporaryDirectory.path(), "short.bin", content));

        const MemoryMappedFile mappedFile = MemoryMappedFile::open(filePath);
        ASSERT_TRUE(mappedFile.isValid()) << "源文件映射失败";

        LoopbackPair pair = makeLoopbackPair();
        ASSERT_TRUE(pair.isValid()) << "回环连接建立失败";

        // 接收方持续排空，确保「发到末尾」不是因为发送缓冲区满
        std::string receivedBytes;
        const int   clientDescriptor = pair.client;
        std::thread receiver([clientDescriptor, &receivedBytes]
        {
            receiveExactly(clientDescriptor, receivedBytes, kFileBytes, kWaitTimeoutMilliseconds);
        });

        // 第一次发起就会发到文件末尾为止，之后再发起必然拿到 0
        std::size_t sentByteCount = 0;
        bool        hasReachedEnd = false;
        const auto  deadline      = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitTimeoutMilliseconds);
        const NativeFileHandle nativeHandle = nativeHandleFor(mappedFile);
        while (sentByteCount < kRequestedByteCount && std::chrono::steady_clock::now() < deadline)
        {
            const ssize_t chunk = Socket::sendFileChunk(pair.server, nativeHandle, sentByteCount, kRequestedByteCount - sentByteCount);
            if (chunk > 0)
            {
                sentByteCount += static_cast<std::size_t>(chunk);
                continue;
            }
            if (chunk == 0)
            {
                hasReachedEnd = true;
                break;
            }
            if (PlatformError::lastSocketErrorCode() == PlatformError::kWouldBlock)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            FAIL() << "零拷贝发送报错：" << PlatformError::message(PlatformError::lastSocketErrorCode());
        }
        receiver.join();

        EXPECT_EQ(sentByteCount, kFileBytes) << "越过文件末尾的请求应只发到文件末尾为止";
        EXPECT_TRUE(hasReachedEnd) << "文件末尾没有以返回 0 上报，调用方会按 kWouldBlock 原地空转";
        expectBytesEqual(receivedBytes, content);
    }

    /**
     * @brief 对端不读时零拷贝发送必须尽快返回（部分写或 kWouldBlock），不得把调用线程吊住
     * @details 这是零拷贝路径能进事件循环的前提：一旦按「发完才返回」实现，一个慢消费者就能把整个
     *          循环停摆（Windows 的 TransmitFile 正是如此，本引擎因此没有采用它）。用例用「不读的
     *          对端 + 极小发送缓冲区 + 大文件」构造该情形，以时间为上界断言必须返回；部分写与
     *          kWouldBlock 两种形态都接受
     */
    TEST(SocketZeroCopySend, DoesNotBlockOnSlowPeer)
    {
        constexpr std::size_t kBigFileBytes = 4 * 1024 * 1024;

        const TemporaryDirectory temporaryDirectory("ZeroCopySendSlowPeer");
        const std::string        content  = makeDeterministicBytes(kBigFileBytes);
        const std::string        filePath = (temporaryDirectory.path() / "big.bin").string();
        ASSERT_TRUE(writeBinaryFile(temporaryDirectory.path(), "big.bin", content));

        const MemoryMappedFile mappedFile = MemoryMappedFile::open(filePath);
        ASSERT_TRUE(mappedFile.isValid()) << "源文件映射失败";

        LoopbackPair pair = makeLoopbackPair();
        ASSERT_TRUE(pair.isValid()) << "回环连接建立失败";
        ASSERT_TRUE(shrinkSendBuffer(pair.server)) << "发送缓冲区没能压到最小值，本用例的前提不成立";

        // 对端一个字节都不读，发送缓冲区很快被填满；此时调用必须返回而不是等在那里。
        // 结果放进堆上的共享状态：真阻塞时线程会被 detach，栈上的量已经不存在了
        struct SendOutcome
        {
            std::atomic<bool>    hasReturned{false};    ///< 调用是否已返回
            std::atomic<ssize_t> byteCount{0};          ///< 返回值
            std::atomic<int>     errorCode{0};          ///< 返回 ≤0 时的最近错误码
        };

        const auto           outcome          = std::make_shared<SendOutcome>();
        const int            serverDescriptor = pair.server;
        const NativeFileHandle nativeHandle   = nativeHandleFor(mappedFile);
        std::thread          sender([serverDescriptor, nativeHandle, outcome]
        {
            outcome->byteCount.store(Socket::sendFileChunk(serverDescriptor, nativeHandle, 0, kBigFileBytes));
            outcome->errorCode.store(PlatformError::lastSocketErrorCode());
            outcome->hasReturned.store(true);
        });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitTimeoutMilliseconds);
        while (!outcome->hasReturned.load() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        if (!outcome->hasReturned.load())
        {
            // 超时后不能 join（会连主线程一起吊死）：detach 让它随测试进程结束
            sender.detach();
            FAIL() << "对端不读时零拷贝发送在 " << kWaitTimeoutMilliseconds << "ms 内没有返回：该调用把线程吊住了";
        }
        sender.join();

        const ssize_t chunkResult = outcome->byteCount.load();
        const int     errorCode   = outcome->errorCode.load();
        EXPECT_TRUE(chunkResult > 0 || errorCode == PlatformError::kWouldBlock)
                << "返回形态既不是部分写也不是 kWouldBlock：" << PlatformError::message(errorCode);
        EXPECT_LT(chunkResult, static_cast<ssize_t>(kBigFileBytes)) << "缓冲区被压到 4 KiB 却报称整份发出，进度反推有误";
    }
} // namespace AsynGyanis::Platform

#endif
