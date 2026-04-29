/**
 * @file TcpStream.h
 * @brief Buffered TCP stream over AsyncSocket with higher-level read/write
 * @copyright Copyright (c) 2026
 */
#ifndef NET_TCPSTREAM_H
#define NET_TCPSTREAM_H

#include "Core/AsyncSocket.h"
#include "Core/Task.h"

#include <cstddef>
#include <string>
#include <vector>

namespace Net
{

    class TcpStream
    {
    public:
        explicit TcpStream(Core::AsyncSocket socket);

        TcpStream(const TcpStream &)            = delete;
        TcpStream &operator=(const TcpStream &) = delete;
        TcpStream(TcpStream &&)                 = default;
        TcpStream &operator=(TcpStream &&)      = default;

        ~TcpStream() = default;

        Core::Task<ssize_t>     read(void *buf, size_t len);
        Core::Task<>            readExact(void *buf, size_t len);
        Core::Task<std::string> readUntil(char delimiter);
        Core::Task<ssize_t>     write(const void *buf, size_t len);
        Core::Task<>            writeAll(const void *buf, size_t len);
        void                    close();

        Core::AsyncSocket &socket();

    private:
        Core::Task<> fillBuffer();

        Core::AsyncSocket m_socket;
        std::vector<char> m_readBuffer;
        size_t            m_readPos;
    };

}

#endif
