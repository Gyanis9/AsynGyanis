#include "HttpServer.h"
#include "Core/Task.h"
#include "FileSender.h"
#include "HttpSession.h"

#include <filesystem>
#include <fstream>

namespace Net
{

    HttpServer::HttpServer(Core::EventLoop &loop, const Core::InetAddress &addr) :
        TcpServer(loop, addr)
    {
    }

    Router &HttpServer::router()
    {
        return m_router;
    }

    std::shared_ptr<Core::Connection> HttpServer::createConnection(Core::AsyncSocket socket)
    {
        return std::make_shared<HttpSession>(m_loop, std::move(socket), m_router);
    }

    void HttpServer::staticFileDir(const std::string &path)
    {
        m_staticDir = path;

        // 注册通配符兜底路由：仅在所有其他路由都不匹配时触发
        m_router.get("*", [path](HttpRequest &req, HttpResponse &res) -> Core::Task<>
        {
            if (req.method() != HttpMethod::GET && req.method() != HttpMethod::HEAD)
            {
                res.setStatus(405);
                res.setBody("Method Not Allowed");
                res.setHeader("content-type", "text/plain");
                co_return;
            }

            // 路径安全检查：防止目录穿越攻击（../ 等）
            const std::string requestPath = req.path();
            if (requestPath.find("..") != std::string::npos)
            {
                res.setStatus(403);
                res.setBody("Forbidden");
                res.setHeader("content-type", "text/plain");
                co_return;
            }

            std::filesystem::path filePath = std::filesystem::path(path) / requestPath.substr(1);

            std::error_code ec;
            if (!std::filesystem::exists(filePath, ec) || !std::filesystem::is_regular_file(filePath, ec))
            {
                res.setStatus(404);
                res.setBody("Not Found");
                res.setHeader("content-type", "text/plain");
                co_return;
            }

            const std::string mimeType = FileSender::contentTypeForFile(filePath.string());
            const auto        fileSize = std::filesystem::file_size(filePath, ec);

            if (ec)
            {
                res.setStatus(500);
                res.setBody("Internal Server Error");
                res.setHeader("content-type", "text/plain");
                co_return;
            }

            // 读取文件内容并设置响应
            std::string body;
            body.resize(fileSize);
            if (std::ifstream file(filePath, std::ios::binary); !file.read(body.data(), static_cast<std::streamsize>(fileSize)))
            {
                res.setStatus(500);
                res.setBody("Internal Server Error");
                res.setHeader("content-type", "text/plain");
                co_return;
            }

            res.setStatus(200);
            res.setHeader("content-type", mimeType);
            res.setBody(std::move(body));
            co_return;
        });
    }

}
