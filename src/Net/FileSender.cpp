#include "FileSender.h"

#include "Core/EpollAwaiter.h"
#include "Core/EventLoop.h"
#include "Platform/Platform.h"

#include <cerrno>
#include <unordered_map>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <sys/sendfile.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace Net
{

    Core::Task<> FileSender::sendFile(Core::EventLoop &loop, TcpStream &stream, const std::string &filePath)
    {
        const int sockFd = stream.socket().fd();

#ifdef _WIN32
        // Windows: 使用 TransmitFile
        HANDLE hFile = CreateFileA(
            filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
            co_return;

        LARGE_INTEGER fileSize;
        if (!GetFileSizeEx(hFile, &fileSize))
        {
            CloseHandle(hFile);
            co_return;
        }

        size_t remaining = static_cast<size_t>(fileSize.QuadPart);

        while (remaining > 0)
        {
            const DWORD toSend = static_cast<DWORD>(std::min(remaining, static_cast<size_t>(ULONG_MAX)));
            const BOOL  ok = TransmitFile(
                static_cast<SOCKET>(sockFd), hFile, toSend, 0, nullptr, nullptr, 0);

            if (ok)
            {
                remaining -= toSend;
                continue;
            }

            const int err = ASYN_ERRNO;
            if (err == ASYN_EAGAIN || err == ASYN_EWOULDBLOCK)
            {
                co_await Core::EpollAwaiter(loop.epoll(), sockFd, EPOLLOUT);
                continue;
            }
            break; // 其他错误
        }

        CloseHandle(hFile);
#else
        // Linux: 使用 sendfile
        const int fileFd = ::open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fileFd < 0)
            co_return;

        // RAII guard: ensure fileFd is always closed, even on coroutine cancellation
        struct FileGuard
        {
            int fd;

            ~FileGuard()
            {
                if (fd >= 0)
                    ::close(fd);
            }
        } guard{fileFd};

        struct stat st{};
        if (::fstat(fileFd, &st) < 0)
            co_return;

        size_t remaining = static_cast<size_t>(st.st_size);

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
#endif
    }

    const char *FileSender::contentTypeForFile(const std::string &filePath)
    {
        static const std::unordered_map<std::string_view, const char *> kMimeTypes = {
                {".html", "text/html"},
                {".htm", "text/html"},
                {".css", "text/css"},
                {".js", "application/javascript"},
                {".mjs", "application/javascript"},
                {".json", "application/json"},
                {".xml", "application/xml"},
                {".txt", "text/plain"},
                {".pdf", "application/pdf"},
                {".png", "image/png"},
                {".jpg", "image/jpeg"},
                {".jpeg", "image/jpeg"},
                {".gif", "image/gif"},
                {".svg", "image/svg+xml"},
                {".ico", "image/x-icon"},
                {".webp", "image/webp"},
                {".woff", "font/woff"},
                {".woff2", "font/woff2"},
                {".ttf", "font/ttf"},
                {".wasm", "application/wasm"},
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
