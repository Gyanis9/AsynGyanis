#include "FileSender.h"

#include "Core/EpollAwaiter.h"
#include "Core/EventLoop.h"

#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>

namespace Net
{

    Core::Task<> FileSender::sendFile(Core::EventLoop &loop, TcpStream &stream, const std::string &filePath)
    {
        const int fileFd = ::open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fileFd < 0)
            co_return;

        struct stat st{};
        if (::fstat(fileFd, &st) < 0)
        {
            ::close(fileFd);
            co_return;
        }

        const int sockFd = stream.socket().fd();
        size_t    remaining = static_cast<size_t>(st.st_size);

        while (remaining > 0)
        {
            const ssize_t n = ::sendfile(sockFd, fileFd, nullptr, remaining);
            if (n > 0)
            {
                remaining -= static_cast<size_t>(n);
                continue;
            }
            if (n == 0)
                break;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                co_await Core::EpollAwaiter(loop.epoll(), sockFd, EPOLLOUT);
                continue;
            }
            if (errno == EINTR)
                continue;
            break;
        }

        ::close(fileFd);
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
