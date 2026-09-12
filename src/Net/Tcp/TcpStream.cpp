#include "Net/Tcp/TcpStream.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/SystemException.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 单次底层接收的缓冲区容量，单位字节
         *
         * @details 4KB 与常见 MTU 与内核 socket 缓冲的粒度匹配：再大也只是把未被发送的数据
         *          留在内核里，再小则会增加系统调用次数。
         */
        constexpr std::size_t kReadBufferCapacity = 4096;
    } // namespace

    TcpStream::TcpStream(Core::AsyncSocket socket) :
        m_socket(std::move(socket)),
        m_readBuffer(kReadBufferCapacity),
        m_readPosition(kReadBufferCapacity)
    {
        // 缓冲区先按上限分配好，同时把消费位置直接摆在末尾：判定「已耗尽」的条件是
        // m_readPosition >= m_readBuffer.size()，这样首次 read() 必定去底层收数据。
        // 若初始位置写成 0，第一次读会把这块尚未填充的缓冲区当成有效数据交给上层
    }

    Core::Task<ssize_t> TcpStream::read(void *const buffer, const std::size_t length)
    {
        // 零长度读取没有语义，直接成功返回，避免白白挂起一次协程
        if (length == 0)
        {
            co_return 0;
        }

        // 缓冲区已被消费完才向底层补货；补货后仍为空说明对端正常关闭（EOF）
        if (m_readPosition >= m_readBuffer.size())
        {
            co_await fillBuffer();
            if (m_readBuffer.empty())
            {
                co_return 0;
            }
        }

        // 本次只搬运「上层要的量」与「缓冲区剩余量」的较小值，多余的留在缓冲区给下次读取
        const std::size_t availableLength = m_readBuffer.size() - m_readPosition;
        const std::size_t copyLength      = std::min(length, availableLength);
        std::memcpy(buffer, m_readBuffer.data() + m_readPosition, copyLength);
        m_readPosition += copyLength;

        co_return static_cast<ssize_t>(copyLength);
    }

    Core::Task<> TcpStream::readExact(void *const buffer, const std::size_t length)
    {
        auto       *destination     = static_cast<char *>(buffer);
        std::size_t remainingLength = length;

        while (remainingLength > 0)
        {
            const ssize_t readBytes = co_await read(destination, remainingLength);
            // 0 表示对端已关闭、负数表示读错误：两者都不可能再凑满剩余字节，
            // 把「短读」当作异常上抛，调用方无需自己判断长度是否完整
            if (readBytes <= 0)
            {
                throw Base::Exception("TcpStream::readExact 在读满所需字节前连接已关闭或发生错误");
            }
            destination       += static_cast<std::size_t>(readBytes);
            remainingLength   -= static_cast<std::size_t>(readBytes);
        }

        co_return;
    }

    Core::Task<std::string> TcpStream::readUntil(const char delimiter, const std::size_t maximumSize)
    {
        std::string result;

        while (true)
        {
            // 上限判定放在每轮开头：触顶时把已读到的部分交还，尚未派发的字节仍留在缓冲区里，
            // 不会被丢弃——因此所有截断分支都只按「实际交出的长度」推进消费位置
            if (maximumSize > 0 && result.size() >= maximumSize)
            {
                co_return result;
            }

            // 缓冲区里还有未消费的数据时，先在其中扫描分隔符，命中即可就地返回
            if (m_readPosition < m_readBuffer.size())
            {
                const char *scanBegin = m_readBuffer.data() + m_readPosition;
                const char *scanEnd   = m_readBuffer.data() + m_readBuffer.size();

                // 按字节扫描：memchr 由运行库针对字节匹配做过向量化优化，比逐元素查找更快
                if (const auto *delimiterPointer = static_cast<const char *>(
                        std::memchr(scanBegin, delimiter, static_cast<std::size_t>(scanEnd - scanBegin))))
                {
                    const std::size_t chunkLength = static_cast<std::size_t>(delimiterPointer - scanBegin);
                    if (maximumSize > 0 && result.size() + chunkLength > maximumSize)
                    {
                        const std::size_t truncateLength = maximumSize - result.size();
                        result.append(scanBegin, truncateLength);
                        // 截断后也要推进消费位置，否则缓冲区里这段字节会被下一次读取重复交出
                        m_readPosition += truncateLength;
                        co_return result;
                    }
                    result.append(scanBegin, chunkLength);
                    // 分隔符本身已被读到，一并跳过：它不属于返回内容
                    m_readPosition += chunkLength + 1;
                    co_return result;
                }

                // 未命中分隔符：整段追加后继续补货，直到遇到分隔符、EOF 或触顶
                const std::size_t appendLength = static_cast<std::size_t>(scanEnd - scanBegin);
                if (maximumSize > 0 && result.size() + appendLength > maximumSize)
                {
                    const std::size_t truncateLength = maximumSize - result.size();
                    result.append(scanBegin, truncateLength);
                    m_readPosition += truncateLength;
                    co_return result;
                }
                result.append(scanBegin, appendLength);
                m_readPosition = m_readBuffer.size();
            }

            try
            {
                co_await fillBuffer();
            }
            catch (const Base::SystemException &)
            {
                // 补货失败说明连接已不可用：按「读到哪算哪」返回，让上层拿完整的前缀去解析，
                // 原始错误码由 fillBuffer 的抛出点保留，不在这里改写
                co_return result;
            }

            // 缓冲区被清空表示对端正常关闭且没再给数据：EOF，返回已读到的部分
            if (m_readBuffer.empty())
            {
                co_return result;
            }
        }
    }

    Core::Task<ssize_t> TcpStream::write(const void *const buffer, const std::size_t length) const
    {
        // 写入不经缓冲区：套接字发送缓冲由内核管理，这里的短写由 writeAll 负责补偿
        co_return co_await m_socket.asyncSend(buffer, length);
    }

    Core::Task<> TcpStream::writeAll(const void *const buffer, const std::size_t length) const
    {
        auto       *source          = static_cast<const char *>(buffer);
        std::size_t remainingLength = length;

        while (remainingLength > 0)
        {
            const ssize_t sentBytes = co_await m_socket.asyncSend(source, remainingLength);
            // asyncSend 返回 0 或负数表示连接已断开或发送失败：剩余字节无处可去，直接上抛
            if (sentBytes <= 0)
            {
                throw Base::Exception("TcpStream::writeAll 发送失败或连接已关闭");
            }
            source          += static_cast<std::size_t>(sentBytes);
            remainingLength -= static_cast<std::size_t>(sentBytes);
        }

        co_return;
    }

    void TcpStream::close()
    {
        // 丢弃未派发的缓冲数据：连接已关，残留字节没有再次交给上层的意义
        m_readBuffer.clear();
        m_readPosition = 0;
        m_socket.close();
    }

    Core::AsyncSocket &TcpStream::socket() noexcept
    {
        return m_socket;
    }

    Core::Task<> TcpStream::fillBuffer()
    {
        // 每次补货都重新占满容量并把消费位置归零，调用方只需看缓冲区长度
        m_readBuffer.resize(kReadBufferCapacity);
        m_readPosition = 0;

        const ssize_t receivedBytes = co_await m_socket.asyncReceive(m_readBuffer.data(), m_readBuffer.size());
        if (receivedBytes > 0)
        {
            // 收缩到实际收到的长度：readUntil 与 read 都以 size() 作为有效数据末端
            m_readBuffer.resize(static_cast<std::size_t>(receivedBytes));
            co_return;
        }

        // 空缓冲区是「EOF 或错误」的共用信号，由调用方按上下文区分
        m_readBuffer.clear();
        if (receivedBytes < 0)
        {
            throw Base::SystemException("TcpStream::fillBuffer 底层接收失败");
        }
        // receivedBytes == 0：对端正常关闭，保持缓冲区为空
        co_return;
    }

} // namespace AsynGyanis::Net
