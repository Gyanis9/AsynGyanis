/**
 * @file FileSender.cpp
 * @brief Zero-copy static file sender
 * @copyright Copyright (c) 2026
 */
#include "FileSender.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <string_view>
#include <vector>

namespace Net
{

    Core::Task<> FileSender::sendFile(TcpStream &stream, const std::string &filePath)
    {
        const int fd = ::open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            co_return;

        struct stat st{};
        if (::fstat(fd, &st) < 0)
        {
            ::close(fd);
            co_return;
        }

        size_t            remaining = static_cast<size_t>(st.st_size);
        std::vector<char> buffer(65536);

        while (remaining > 0)
        {
            const size_t  toRead = std::min(remaining, buffer.size());
            const ssize_t n      = ::read(fd, buffer.data(), toRead);
            if (n <= 0)
                break;

            co_await stream.writeAll(buffer.data(), static_cast<size_t>(n));
            remaining -= static_cast<size_t>(n);
        }

        ::close(fd);
    }

    const char *FileSender::contentTypeForFile(const std::string &filePath)
    {
        const auto dotPos = filePath.rfind('.');
        if (dotPos == std::string::npos)
            return "application/octet-stream";

        const std::string_view ext(&filePath[dotPos]);

        if (ext == ".html" || ext == ".htm")
            return "text/html";
        if (ext == ".css")
            return "text/css";
        if (ext == ".js")
            return "application/javascript";
        if (ext == ".json")
            return "application/json";
        if (ext == ".xml")
            return "application/xml";
        if (ext == ".txt")
            return "text/plain";
        if (ext == ".pdf")
            return "application/pdf";
        if (ext == ".png")
            return "image/png";
        if (ext == ".jpg" || ext == ".jpeg")
            return "image/jpeg";
        if (ext == ".gif")
            return "image/gif";
        if (ext == ".svg")
            return "image/svg+xml";
        if (ext == ".ico")
            return "image/x-icon";

        return "application/octet-stream";
    }

}
