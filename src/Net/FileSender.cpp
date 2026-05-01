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

        // RAII guard: ensure fileFd is always closed, even on coroutine cancellation
        struct FileGuard
        {
            int fd;
            ~FileGuard() { if (fd >= 0) ::close(fd); }
        } guard{fileFd};

        struct stat st{};
        if (::fstat(fileFd, &st) < 0)
            co_return;

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
    }

    const char *FileSender::contentTypeForFile(const std::string &filePath)
    {
        static const std::unordered_map<std::string_view, const char *> kMimeTypes = {
            {".html",  "text/html"},
            {".htm",   "text/html"},
            {".css",   "text/css"},
            {".js",    "application/javascript"},
            {".mjs",   "application/javascript"},
            {".json",  "application/json"},
            {".xml",   "application/xml"},
            {".txt",   "text/plain"},
            {".pdf",   "application/pdf"},
            {".png",   "image/png"},
            {".jpg",   "image/jpeg"},
            {".jpeg",  "image/jpeg"},
            {".gif",   "image/gif"},
            {".svg",   "image/svg+xml"},
            {".ico",   "image/x-icon"},
            {".webp",  "image/webp"},
            {".woff",  "font/woff"},
            {".woff2", "font/woff2"},
            {".ttf",   "font/ttf"},
            {".wasm",  "application/wasm"},
        };

        const auto dotPos = filePath.rfind('.');
        if (dotPos == std::string::npos)
            return "application/octet-stream";

        const std::string_view ext(&filePath[dotPos]);

        if (const auto it = kMimeTypes.find(ext); it != kMimeTypes.end())
            return it->second;

        return "application/octet-stream";
    }

}
