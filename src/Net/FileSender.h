/**
 * @file FileSender.h
 * @brief Zero-copy static file sender
 * @copyright Copyright (c) 2026
 */
#ifndef NET_FILESENDER_H
#define NET_FILESENDER_H

#include "Core/Task.h"
#include "TcpStream.h"

#include <string>

namespace Net
{

    class FileSender
    {
    public:
        FileSender() = delete;

        static Core::Task<> sendFile(TcpStream &stream, const std::string &filePath);

    private:
        static const char *contentTypeForFile(const std::string &filePath);
    };

}

#endif
