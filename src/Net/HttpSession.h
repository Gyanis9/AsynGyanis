#ifndef NET_HTTPSESSION_H
#define NET_HTTPSESSION_H

#include "Core/Connection.h"
#include "Core/Task.h"
#include "HttpParser.h"
#include "Router.h"

#include <vector>

namespace Net
{
    class HttpSession : public Core::Connection
    {
    public:
        HttpSession(Core::EventLoop &loop, Core::AsyncSocket socket, Router &router);

        Core::Task<> start() override;

    private:
        bool shouldKeepAlive(const HttpRequest &req, const HttpResponse &res) const;

        Router &                  m_router;
        HttpParser                m_parser;
        std::vector<char>         m_recvBuffer;
        static constexpr int      m_recvBufferSize = 8192;
    };

}

#endif
