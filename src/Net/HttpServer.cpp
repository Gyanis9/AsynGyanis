/**
 * @file HttpServer.cpp
 * @brief HTTP 服务器，在 TcpServer 之上装配路由与会话
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "HttpServer.h"
#include "Core/Task.h"
#include "FileSender.h"
#include "HttpSession.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace Net
{

    HttpServer::HttpServer(Core::EventLoop &loop, const Core::InetAddress &address) :
        TcpServer(loop, address)
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
        m_router.get("*", [path](HttpRequest &request, HttpResponse &response) -> Core::Task<>
        {
            if (request.method() != HttpMethod::GET && request.method() != HttpMethod::HEAD)
            {
                response.setStatus(405);
                response.setBody("Method Not Allowed");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            // 路径安全检查：使用规范化路径 + 根目录包含校验，防止目录穿越（../）、
            // 绝对路径注入（如 "//etc/passwd"）、Windows 盘符/UNC 注入（如 "/c:/..."）等攻击
            std::error_code             ec;
            const std::filesystem::path rootDir = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
            if (ec)
            {
                response.setStatus(500);
                response.setBody("Internal Server Error");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            // relative_path() 去除请求路径中的根名/根目录（盘符、前导 '/'），
            // weakly_canonical 归一化 ".." 并解析已存在部分的符号链接
            const std::filesystem::path requestRelative = std::filesystem::path(request.path()).relative_path();
            const std::filesystem::path filePath        = std::filesystem::weakly_canonical(rootDir / requestRelative, ec);
            if (ec)
            {
                response.setStatus(404);
                response.setBody("Not Found");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            // 校验最终路径仍位于静态根目录之下，否则视为越权访问
            const auto rootMismatch = std::mismatch(rootDir.begin(), rootDir.end(), filePath.begin(), filePath.end());
            if (rootMismatch.first != rootDir.end())
            {
                response.setStatus(403);
                response.setBody("Forbidden");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            if (!std::filesystem::exists(filePath, ec) || !std::filesystem::is_regular_file(filePath, ec))
            {
                response.setStatus(404);
                response.setBody("Not Found");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            const std::string mimeType = FileSender::contentTypeForFile(filePath.string());
            const auto        fileSize = std::filesystem::file_size(filePath, ec);

            if (ec)
            {
                response.setStatus(500);
                response.setBody("Internal Server Error");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            // 内存保护：拒绝将超大文件整体读入内存，避免内存耗尽 DoS
            static constexpr std::uintmax_t kMaxStaticFileSize = 64ull * 1024 * 1024; // 64 MB
            if (fileSize > kMaxStaticFileSize)
            {
                response.setStatus(413);
                response.setBody("Payload Too Large");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            // 读取文件内容并设置响应
            std::string body;
            body.resize(fileSize);
            if (std::ifstream file(filePath, std::ios::binary); !file.read(body.data(), static_cast<std::streamsize>(fileSize)))
            {
                response.setStatus(500);
                response.setBody("Internal Server Error");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            response.setStatus(200);
            response.setHeader("content-type", mimeType);
            response.setBody(std::move(body));
            co_return;
        });
    }

}
