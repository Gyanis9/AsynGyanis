#include "Net/Tcp/TcpClient.h"

#include "Base/Exception/Exception.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/AsyncResolver.h"
#include "Core/Socket/AsyncSocket.h"

#include <memory>
#include <vector>

namespace AsynGyanis::Net
{
    Core::Task<std::unique_ptr<TcpStream>> TcpClient::connect(Core::EventLoop &loop, const std::string_view host, const uint16_t port)
    {
        if (host.empty())
        {
            co_return nullptr;
        }

        // 第一步：异步解析主机名
        const std::vector<Core::InetAddress> addresses = co_await Core::AsyncResolver::resolve(loop, host, port);
        if (addresses.empty())
        {
            co_return nullptr;
        }

        // 第二步：逐个尝试连接，首个成功即返回
        for (const Core::InetAddress &addr: addresses)
        {
            auto socket = Core::AsyncSocket::create(loop);
            try
            {
                co_await socket.asyncConnect(addr);
            } catch (const Base::Exception &)
            {
                // 当前地址连接失败：自动关闭该 socket（析构时 close），继续试下一个
                continue;
            }

            // 连接成功：把已连上的 socket 收入 TcpStream
            co_return std::make_unique<TcpStream>(std::move(socket));
        }

        // 所有地址都连不上
        co_return nullptr;
    }
} // namespace AsynGyanis::Net