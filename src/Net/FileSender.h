/**
 * @file FileSender.h
 * @brief Zero-copy static file sender using sendfile(2)
 * @copyright Copyright (c) 2026
 */
#ifndef NET_FILESENDER_H
#define NET_FILESENDER_H

#include "Core/Task.h"
#include "TcpStream.h"

#include <string>

namespace Core
{
    class EventLoop;
}

namespace Net
{

    class FileSender
    {
    public:
        FileSender() = delete;

        static Core::Task<> sendFile(Core::EventLoop &loop, TcpStream &stream, const std::string &filePath);

    private:
        static const char *contentTypeForFile(const std::string &filePath);
    };

}

#endif
