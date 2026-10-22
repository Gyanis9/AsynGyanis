/**
 * @file HttpClientConnections.cpp
 * @brief 出站明文/TLS 连接建立、代理拨号与 CONNECT 隧道
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 域名解析、带时限的拨号、TLS 握手（直连 / 代理 TLS / 隧道内 TLS）、CONNECT
 *          隧道与代理鉴权都在这里；请求编排、复用决策与响应处理留在 HttpClient.cpp。
 */

#include "Net/Http/Client/HttpClientSupport.h"

#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/Client/HttpProxyPumpedTls.h"

#include "Base/Exception/Exception.h"
#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/DeadlineGuard.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/AsyncResolver.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Socket/ConnectionRace.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/TlsContext.h"
#include "Core/Tls/TlsPolicy.h"
#include "Core/Tls/TlsSocket.h"
#include "Net/Tcp/TcpStream.h"
#include "Platform/IO/Socket.h"

#include <openssl/ssl.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    using HttpClientDetail::remainingBudget;

    namespace
    {
        /// 进程级缓存的客户端 SSL_CTX（首次 HTTPS 请求时创建）
        static SSL_CTX *clientSslCtx()
        {
            // 函数内 static 指针 + 堆分配，确保进程退出时不参与全局析构顺序
            static SSL_CTX *ctx = []() -> SSL_CTX *
            {
                auto *c = SSL_CTX_new(TLS_client_method());
                if (!c)
                    return nullptr;
                // 加载系统默认 CA 证书库（Linux /etc/ssl/certs, Windows 系统存储）
                SSL_CTX_set_default_verify_paths(c);
                // 要求校验服务端证书（但暂不设回调——基本校验由 OpenSSL 默认完成）
                SSL_CTX_set_verify(c, SSL_VERIFY_PEER, nullptr);
                return c;
            }();
            return ctx;
        }

        /// 判断主机名是不是 IP 字面量：IP 与 DNS 名的证书校验规则不同，必须分开处理
        bool isIpLiteral(const std::string &host)
        {
            return Core::InetAddress::parseLiteral(host, 0).has_value();
        }

        /**
         * @brief 建一个客户端 SSL 会话并接上 TLS 上下文
         * @param clientTls 调用方提供的上下文；空时用进程级缓存的那一份
         * @param failureReason 失败原因输出
         * @return SSL* 新建的会话；失败为空（原因已写进 failureReason）
         */
        SSL *createClientSsl(const Core::TlsContext *clientTls, std::string &failureReason)
        {
            auto *ctx = clientTls != nullptr ? clientTls->nativeHandle() : clientSslCtx();
            if (!ctx)
            {
                failureReason = "TLS 上下文创建失败：OpenSSL 没能给出 client method，通常是库未正确初始化";
                return nullptr;
            }
            SSL *ssl = SSL_new(ctx);
            if (!ssl)
            {
                failureReason = "TLS 会话对象创建失败：OpenSSL 内存不足";
            }
            return ssl;
        }

        /**
         * @brief 在已绑定描述符并配置好的会话上按剩余预算完成握手
         * @param tlsSocket 待握手的会话
         * @param loop 所属事件循环
         * @param startedAt 请求开始时刻
         * @param requestTimeout 请求总时限
         * @param peerLabel 日志里的对端称呼（目标主机或代理）
         * @param deadlineText 时限已经耗尽的失败文案
         * @param failureText 握手失败的失败文案（不含底层原因）
         * @param deadlineTag 握手时限守卫的阶段名
         * @param failureReason 失败原因输出
         * @return std::optional<std::unique_ptr<Core::TlsSocket>> 握手成功交出会话；失败为空
         */
        Core::Task<std::optional<std::unique_ptr<Core::TlsSocket>>> finishTlsHandshake(std::unique_ptr<Core::TlsSocket> tlsSocket, Core::EventLoop &loop,
                                                                                       const std::chrono::steady_clock::time_point startedAt,
                                                                                       const std::chrono::milliseconds requestTimeout, const std::string_view peerLabel,
                                                                                       const std::string &deadlineText, const std::string &failureText,
                                                                                       const std::string_view deadlineTag, std::string &failureReason)
        {
            const std::optional<std::chrono::milliseconds> handshakeBudget = remainingBudget(startedAt, requestTimeout);
            if (!handshakeBudget.has_value())
            {
                tlsSocket->close();
                failureReason = deadlineText;
                co_return std::nullopt;
            }
            const Core::DeadlineGuard<Core::TlsSocket> handshakeDeadline(loop, *tlsSocket, *handshakeBudget, deadlineTag);
            try
            {
                co_await tlsSocket->handshake();
            } catch (const Base::Exception &failure)
            {
                LOG_WARN_FMT("HttpClient: 与 {} 的 TLS 握手失败。底层原因：{}", peerLabel, failure.what());
                failureReason = failureText;
                co_return std::nullopt;
            }
            co_return std::optional<std::unique_ptr<Core::TlsSocket>>{std::move(tlsSocket)};
        }
        Core::Task<std::optional<Core::AsyncSocket>> connectWithDeadline(Core::EventLoop &loop, const std::string &host, const std::uint16_t port,
                                                                         const std::chrono::milliseconds connectTimeout, std::string &failureReason)
        {
            // DNS 也吃这份连接预算：解析卡住时不能把整条请求拖过自己的时限。解析超时后不再把
            // 整个 connectTimeout 交给建连，剩余多少用多少
            const auto                           resolveDeadline = std::chrono::steady_clock::now() + connectTimeout;
            const auto                           resolveTimeout  = connectTimeout > std::chrono::milliseconds::zero() ? connectTimeout : std::chrono::milliseconds{1};
            const std::vector<Core::InetAddress> addresses       = co_await Core::AsyncResolver::resolve(loop, host, port, resolveTimeout);
            if (addresses.empty())
            {
                failureReason = "解析地址失败或超时：没能把「" + host + "」解析成可用地址";
                co_return std::nullopt;
            }

            const auto remainingConnectBudget = std::chrono::duration_cast<std::chrono::milliseconds>(resolveDeadline - std::chrono::steady_clock::now());
            if (remainingConnectBudget <= std::chrono::milliseconds::zero())
            {
                failureReason = "解析地址耗尽了连接预算：还没开始建连就已到时限（主机 " + host + "）";
                co_return std::nullopt;
            }
            auto connected = co_await Core::connectCandidates(loop, Core::orderForConnectionRace(addresses), remainingConnectBudget);
            if (!connected)
            {
                failureReason = "建立 TCP 连接失败：对端拒绝、不可达或还没连上就超时（目标 " + host + ":" + std::to_string(port) + "）";
                co_return std::nullopt;
            }
            co_return std::move(connected->socket);
        }

        /// 经代理拨号失败时把「连不上代理」这一层写进原因；直连失败原样留给上层
        void noteProxyDialFailure(std::string &failureReason, const std::string &dialHost, const std::uint16_t dialPort, const bool isProxied)
        {
            if (isProxied)
            {
                failureReason = "连不上代理「" + dialHost + ":" + std::to_string(dialPort) + "」：" + failureReason;
            }
        }

        /// 本次拨号目标：经代理拨代理，直连拨目标（TLS 与池键不受影响）
        std::pair<std::string, std::uint16_t> dialTargetForConnection(const std::string &targetHost, const std::uint16_t targetPort,
                                                                      const std::optional<HttpProxyRoute> &proxyRoute)
        {
            return proxyRoute.has_value() ? std::pair<std::string, std::uint16_t>{proxyRoute->host, proxyRoute->port}
                                          : std::pair<std::string, std::uint16_t>{targetHost, targetPort};
        }

        /**
         * @brief 给客户端 SSL 配 SNI、主机名校验与（可选的）ALPN 声明
         * @param ssl 待配置的 SSL 对象
         * @param host 校验与 SNI 用的主机名（IP 字面量走 IP 校验、域名走 DNS 名校验）
         * @param withAlpn true = 声明 h2/http1.1（到目标的一路）；false = 不声明（到代理的一路）
         * @return true 配置完成；false 表示 ALPN 列表被 OpenSSL 拒（唯一可失败的步骤）
         * @details 链校验只证明「证书由受信 CA 签发」，主机名校验才证明「签发的对象就是这台主机」——
         *          少了它任何受信 CA 给他域签的证书都能冒充目标（CWE-297）。ALPN 的线格式是
         *          「单字节长度 + 协议名」的串联而非逗号分隔；SSL_set_alpn_protos 的返回值是反的
         *          （0 才是成功），名字不用废弃的 ...protocols 写法
         */
        bool configureClientSsl(SSL *ssl, const std::string &host, const bool withAlpn)
        {
            SSL_set_tlsext_host_name(ssl, host.c_str());
            // IP 字面量与 DNS 名的匹配规则不同，必须分开设置
            if (isIpLiteral(host))
            {
                X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host.c_str());
            } else
            {
                SSL_set1_host(ssl, host.c_str());
            }
            if (!withAlpn)
            {
                return true;
            }
            static constexpr unsigned char kAlpnProtocolList[] = {
                    2U, 'h', '2', 8U, 'h', 't', 't', 'p', '/', '1', '.', '1',
            };
            return ::SSL_set_alpn_protos(ssl, kAlpnProtocolList, sizeof(kAlpnProtocolList)) == 0;
        }

        /**
         * @brief 在已连上的字节流（直连 socket，或明文隧道之后）上做目标的 TLS 握手
         * @param socket 已连到目标（或其明文隧道已打通）的套接字（所有权移入返回的 TlsSocket）
         * @param loop 所属事件循环
         * @param u 已拆开的 URL（主机与端口是 SNI 与校验的依据）
         * @param clientTls 复用的 TLS 客户端上下文，可空
         * @param startedAt 本次请求的开始时刻
         * @param requestTimeout 整体时限
         * @param failureReason 输出：失败在哪一段
         * @return std::optional<std::unique_ptr<Core::TlsSocket>> 握手成功交出 TlsSocket；失败返回空
         */
        Core::Task<std::optional<std::unique_ptr<Core::TlsSocket>>> openTlsToTarget(Core::AsyncSocket socket, Core::EventLoop &loop, const ParsedUrl &u,
                                                                                    const Core::TlsContext *clientTls, const std::chrono::steady_clock::time_point startedAt,
                                                                                    const std::chrono::milliseconds requestTimeout, std::string &failureReason)
        {
            // 带池那一路用本实例自己的上下文（策略、信任库、客户端证书都在那上面）；
            // 静态那一路没有承载策略的地方，仍用进程级默认上下文
            SSL *ssl = createClientSsl(clientTls, failureReason);
            if (ssl == nullptr)
            {
                co_return std::nullopt;
            }

            // SSL 必须绑上底层描述符才有 BIO 可用：少了这一步 SSL_connect 立刻失败，
            // 而且错误队列是空的（error:00000000），现场只剩「握手失败」四个字
            if (::SSL_set_fd(ssl, socket.fileDescriptor()) == 0)
            {
                SSL_free(ssl);
                failureReason = "TLS 无法绑定套接字描述符：OpenSSL 的 BIO 创建失败";
                co_return std::nullopt;
            }

            // SNI、主机名校验（CWE-297）与 ALPN 声明：到目标的一路三样都要
            if (!configureClientSsl(ssl, u.host, true))
            {
                SSL_free(ssl);
                failureReason = "TLS 没能设置 ALPN 协议列表：OpenSSL 拒绝了这份写法";
                co_return std::nullopt;
            }

            auto tlsSocket = std::make_unique<Core::TlsSocket>(ssl, loop, std::move(socket), Core::TlsSocket::Role::Client);
            co_return co_await finishTlsHandshake(std::move(tlsSocket), loop, startedAt, requestTimeout, u.host, "本次请求已到时限：还没做 TLS 握手（主机 " + u.host + "）",
                                                  "TLS 握手失败：证书没通过校验、协议或密码套件不匹配，或对端在握手中途收线（主机 " + u.host + "）", "HttpClient", failureReason);
        }

        /**
         * @brief 与 https 代理做一次客户端 TLS 握手（按代理主机名校验；刻意不声明 ALPN）
         * @param socket 已连到代理的套接字（所有权移入返回的 TlsSocket）
         * @param loop 所属事件循环
         * @param route 代理路由（主机与端口用于 SNI 与校验）
         * @param clientTls 复用的 TLS 客户端上下文，可空
         * @param startedAt 本次请求的开始时刻
         * @param requestTimeout 整体时限
         * @param failureReason 输出：失败在哪一段、点名到「代理」这一层
         * @return std::optional<std::unique_ptr<Core::TlsSocket>> 握手成功交出 TlsSocket；失败返回空
         * @details 外层 TLS 的对端是代理而不是目标：SNI 与主机名校验都按代理主机走（目标的证书
         *          在 https 目标的 CONNECT 隧道里由内层 TLS 各自校验）。证书链用的是与目标同一份
         *          TLS 策略（同一 CA 信任库与客户端证书）——独立代理证书不做：要给代理单独一份
         *          信任库与客户端证书，就得先有「代理与目标分属两套身份」的部署形状，本仓没有。
         *          不声明 ALPN 是有意为之：到代理的连接按 HTTP/1.1 语义说话（CONNECT 与绝对形式
         *          都是 h1 的），声明 h2 只会把协商引向本端没实现的通路
         */
        Core::Task<std::optional<std::unique_ptr<Core::TlsSocket>>> openTlsToProxy(Core::AsyncSocket socket, Core::EventLoop &loop, const HttpProxyRoute &route,
                                                                                   const Core::TlsContext *clientTls, const std::chrono::steady_clock::time_point startedAt,
                                                                                   const std::chrono::milliseconds requestTimeout, std::string &failureReason)
        {
            SSL *ssl = createClientSsl(clientTls, failureReason);
            if (ssl == nullptr)
            {
                co_return std::nullopt;
            }
            if (::SSL_set_fd(ssl, socket.fileDescriptor()) == 0)
            {
                SSL_free(ssl);
                failureReason = "TLS 无法绑定套接字描述符：OpenSSL 的 BIO 创建失败";
                co_return std::nullopt;
            }

            // SNI 与主机名校验都按代理主机；不声明 ALPN（到代理的连接恒按 h1 语义）
            static_cast<void>(configureClientSsl(ssl, route.host, false));

            auto              tlsSocket = std::make_unique<Core::TlsSocket>(ssl, loop, std::move(socket), Core::TlsSocket::Role::Client);
            const std::string peerText  = "代理 " + route.host + ":" + std::to_string(route.port);
            co_return co_await finishTlsHandshake(std::move(tlsSocket), loop, startedAt, requestTimeout, peerText, "本次请求已到时限：还没与代理做 TLS 握手（" + peerText + "）",
                                                  "与代理「" + route.host + ":" + std::to_string(route.port) +
                                                          "」的 TLS 握手失败：证书没通过校验、协议或密码套件不匹配，或对端在握手中途收线",
                                                  "HttpClient 代理 TLS", failureReason);
        }

        /**
         * @brief 在外层隧道上建内层 TLS（TLS-in-TLS 的第二层）：按目标校验与协商，字节经泵搬运
         * @param outer 与代理握手完成、CONNECT 已打通的外层传输（所有权移入返回的泵）
         * @param loop 所属事件循环
         * @param u 已拆开的 URL（主机与端口是内层 SNI 与校验的依据）
         * @param clientTls 复用的 TLS 客户端上下文，可空
         * @param startedAt 本次请求的开始时刻
         * @param requestTimeout 整体时限
         * @param failureReason 输出：失败点在哪一段
         * @return std::optional<std::unique_ptr<HttpProxyPumpedTls>> 内层握手成功交出泵；失败返回空
         * @details 内层 SSL 不绑文件描述符——它的收发全走内存 BIO，由泵经外层隧道的明文流搬运；
         *          SNI、主机名校验与 ALPN 照常按目标配置（h2 仍可能在隧道里谈出来）
         */
        Core::Task<std::optional<std::unique_ptr<HttpProxyPumpedTls>>> openInnerTlsThroughTunnel(std::unique_ptr<Core::TlsSocket> outer, Core::EventLoop &loop, const ParsedUrl &u,
                                                                                                 const Core::TlsContext                     *clientTls,
                                                                                                 const std::chrono::steady_clock::time_point startedAt,
                                                                                                 const std::chrono::milliseconds requestTimeout, std::string &failureReason)
        {
            SSL *ssl = createClientSsl(clientTls, failureReason);
            if (ssl == nullptr)
            {
                co_return std::nullopt;
            }

            // SSL_new 不认角色：不显式声明连接类型，SSL_do_handshake 会以
            // 「connection type not set」当场失败（内层恒为本端客户端）
            SSL_set_connect_state(ssl);

            // 内层按目标校验与协商（三样都要：SNI、主机名校验与 ALPN）
            if (!configureClientSsl(ssl, u.host, true))
            {
                SSL_free(ssl);
                failureReason = "TLS 没能设置 ALPN 协议列表：OpenSSL 拒绝了这份写法";
                co_return std::nullopt;
            }

            auto pumpedSocket = std::make_unique<HttpProxyPumpedTls>(HttpProxyPumpedTls::InnerSslHandle(ssl, &HttpProxyPumpedTls::freeInnerSsl), std::move(outer));
            if (!pumpedSocket->isReady())
            {
                pumpedSocket->close();
                failureReason = "内层 TLS 的内存 BIO 创建失败：OpenSSL 内存不足";
                co_return std::nullopt;
            }

            const std::optional<std::chrono::milliseconds> handshakeBudget = remainingBudget(startedAt, requestTimeout);
            if (!handshakeBudget.has_value())
            {
                pumpedSocket->close();
                failureReason = "本次请求已到时限：还没做内层 TLS 握手（主机 " + u.host + "）";
                co_return std::nullopt;
            }
            const Core::DeadlineGuard<HttpProxyPumpedTls> handshakeDeadline(loop, *pumpedSocket, *handshakeBudget, "HttpClient 内层 TLS");
            try
            {
                co_await pumpedSocket->handshake();
            } catch (const Base::Exception &failure)
            {
                LOG_WARN_FMT("HttpClient: 经代理隧道与 {} 的 TLS 握手失败。底层原因：{}", u.host, failure.what());
                failureReason = "经代理隧道与「" + u.host + "」的 TLS 握手失败：证书没通过校验、协议或密码套件不匹配，或隧道在握手中途中断";
                co_return std::nullopt;
            }
            co_return std::optional<std::unique_ptr<HttpProxyPumpedTls>>{std::move(pumpedSocket)};
        }
        /// CONNECT 应答头区的字节上限：头区都不是正文，一个不封顶的读等于让坏代理能拖内存
        constexpr std::size_t kProxyConnectHeadByteLimit = 8192;

        /// 从 CONNECT 应答头里取三位状态码；格式不可解析交回 false（上下文文案由调用方补）
        bool parseConnectResponseStatus(const std::string_view responseHead, int &statusCode) noexcept
        {
            const std::size_t firstSpace = responseHead.find(' ');
            if (firstSpace == std::string_view::npos || responseHead.size() < firstSpace + 4U)
            {
                return false;
            }
            for (std::size_t index = firstSpace + 1U; index < firstSpace + 4U; ++index)
            {
                if (responseHead[index] < '0' || responseHead[index] > '9')
                {
                    return false;
                }
                statusCode = statusCode * 10 + (responseHead[index] - '0');
            }
            return statusCode >= 100 && statusCode <= 599;
        }

        /**
         * @brief 在已连上的代理连接上开一条 CONNECT 隧道
         * @param socket 已连到代理的套接字（成功即从此按隧道字节流用）
         * @param loop 所属事件循环（看门狗挂它）
         * @param targetHost 目标主机（IPv6 不含方括号）
         * @param targetPort 目标端口
         * @param route 代理路由（带凭据时在 CONNECT 上写 Proxy-Authorization）
         * @param budget 本次 CONNECT 应答可花的剩余时限
         * @param failureReason 输出：失败在哪一段、代理给了什么状态码
         * @return true 隧道已建立（2xx 且应答头之后无抢跑字节）
         */
        template<typename StreamT>
        Core::Task<bool> openProxyTunnel(StreamT &socket, Core::EventLoop &loop, const std::string &targetHost, const std::uint16_t targetPort, const HttpProxyRoute &route,
                                         const std::chrono::milliseconds budget, std::string &failureReason)
        {
            std::string targetAuthority = targetHost.find(':') != std::string::npos ? '[' + targetHost + ']' : targetHost;
            targetAuthority += ':';
            targetAuthority += std::to_string(targetPort);

            std::string requestText;
            requestText += "CONNECT " + targetAuthority + " HTTP/1.1\r\n";
            requestText += "Host: " + targetAuthority + "\r\n";
            if (!route.authorizationHeader.empty())
            {
                requestText += "Proxy-Authorization: " + route.authorizationHeader + "\r\n";
            }
            requestText += "\r\n";

            // 写与读都吃剩余预算：看门狗到点关套接字，把挂在读上的等待叫醒（理由同 TLS 握手那一段）
            const auto                         tunnelDeadlineAt = std::chrono::steady_clock::now() + budget;
            const Core::DeadlineGuard<StreamT> tunnelDeadline(loop, socket, budget, "HttpClient 代理隧道");

            // TLS 侧（外层隧道）的收发以异常报失败、明文侧以返回值报失败：两条都折成 false + 原因，
            // 让调用方的一条分支同时管住两种传输
            ssize_t sentCount = 0;
            try
            {
                sentCount = co_await socket.asyncSend(requestText.data(), requestText.size());
            } catch (const Base::Exception &failure)
            {
                failureReason = "连接代理隧道失败：CONNECT 请求发送时外层传输报错（" + std::string(failure.what()) + "）（目标 " + targetAuthority + "）";
                co_return false;
            }
            if (sentCount != static_cast<ssize_t>(requestText.size()))
            {
                failureReason = "连接代理隧道失败：CONNECT 请求没送全或对端已收口（目标 " + targetAuthority + "）";
                co_return false;
            }

            // 只读头区：CONNECT 的应答没有正文，读到空行即止；封顶防坏代理拖内存
            std::string responseHead;
            while (responseHead.find("\r\n\r\n") == std::string::npos)
            {
                char    buffer[2048];
                ssize_t received = 0;
                try
                {
                    received = co_await socket.asyncReceive(buffer, sizeof(buffer));
                } catch (const Base::Exception &failure)
                {
                    failureReason = std::chrono::steady_clock::now() >= tunnelDeadlineAt
                                            ? "等待代理的 CONNECT 应答耗尽了本次请求的剩余时限（目标 " + targetAuthority + "）"
                                            : "等待代理的 CONNECT 应答时外层传输报错（" + std::string(failure.what()) + "）（目标 " + targetAuthority + "）";
                    co_return false;
                }
                if (received <= 0)
                {
                    // 两种成因分开报：看门狗关套接字（到点）与代理自己收线，排查去向完全不同
                    failureReason = std::chrono::steady_clock::now() >= tunnelDeadlineAt ? "等待代理的 CONNECT 应答耗尽了本次请求的剩余时限（目标 " + targetAuthority + "）"
                                                                                         : "代理在 CONNECT 应答前关闭了连接（目标 " + targetAuthority + "）";
                    co_return false;
                }
                responseHead.append(buffer, static_cast<std::size_t>(received));
                if (responseHead.size() > kProxyConnectHeadByteLimit)
                {
                    failureReason = "代理的 CONNECT 应答头超过 " + std::to_string(kProxyConnectHeadByteLimit) + " 字节仍未结束（目标 " + targetAuthority + "）";
                    co_return false;
                }
            }

            const std::size_t headEnd = responseHead.find("\r\n\r\n") + 4U;
            if (responseHead.size() > headEnd)
            {
                // 200 之后紧跟隧道字节是坏代理的抢跑：那几字节进不了 TLS 层也退不回去，宁可当场拒绝
                failureReason = "代理在 CONNECT 应答之后抢跑了 " + std::to_string(responseHead.size() - headEnd) + " 字节（目标 " + targetAuthority + "）";
                co_return false;
            }

            int statusCode = 0;
            if (!parseConnectResponseStatus(responseHead, statusCode))
            {
                failureReason = "代理的 CONNECT 应答没有可解析的状态行（目标 " + targetAuthority + "）";
                co_return false;
            }
            if (statusCode < 200 || statusCode >= 300)
            {
                failureReason = "代理拒绝建立隧道（HTTP " + std::to_string(statusCode) + "）：目标 " + targetAuthority +
                                (statusCode == 407 ? "；代理要求身份验证，本端不会应答 407 挑战——请把凭据写进代理地址（http://用户:口令@主机:端口）" : "");
                co_return false;
            }
            co_return true;
        }

        /// 用剩余预算开 CONNECT 隧道；预算不够或隧道失败都交回 false（原因已写好）
        template<typename StreamT>
        Core::Task<bool> openProxyTunnelWithBudget(StreamT &socket, Core::EventLoop &loop, const ParsedUrl &u, const HttpProxyRoute &route,
                                                   const std::chrono::steady_clock::time_point startedAt, const std::chrono::milliseconds requestTimeout,
                                                   std::string &failureReason)
        {
            const std::optional<std::chrono::milliseconds> tunnelBudget = remainingBudget(startedAt, requestTimeout);
            if (!tunnelBudget.has_value())
            {
                failureReason = "本次请求已到时限：还没建立到「" + u.host + "」的代理隧道";
                co_return false;
            }
            co_return co_await openProxyTunnel(socket, loop, u.host, u.port, route, *tunnelBudget, failureReason);
        }
    } // namespace

    namespace HttpClientDetail
    {
        /**
         * @brief 建一条明文连接
         * @param loop 所属事件循环
         * @param key 目标身份（主机与端口；TLS 位由调用方按 scheme 定）
         * @param failureReason 输出：连不上时点明断在哪一段
         * @param startedAt 本次请求的开始时刻，用于把整体时限摊到剩下那一段
         * @param requestTimeout 整体时限
         * @param proxyRoute 非空即经代理：拨的是代理地址，请求走绝对形式（池键仍是目标）；
         *        路由是 `https://` 代理时连上之后先与代理做一次 TLS 握手再说话
         * @param clientTls 复用的 TLS 客户端上下文（https 代理那一路要用），可空
         * @return std::unique_ptr<HttpOutboundConnection> 连上了就交出连接，连接失败返回空
         */
        Core::Task<std::unique_ptr<HttpOutboundConnection>> establishPlainConnection(Core::EventLoop &loop, const HttpOutboundEndpointKey &key,
                                                                                     const std::chrono::steady_clock::time_point startedAt,
                                                                                     const std::chrono::milliseconds requestTimeout, std::string &failureReason,
                                                                                     const std::optional<HttpProxyRoute> &proxyRoute, const Core::TlsContext *clientTls)
        {
            // 经代理的明文请求拨的是代理地址，池键仍是目标——连接的身份按用途记，拨号按路由走
            const auto [dialHost, dialPort] = dialTargetForConnection(key.host, key.port, proxyRoute);

            const std::optional<std::chrono::milliseconds> connectBudget = remainingBudget(startedAt, requestTimeout);
            if (!connectBudget.has_value())
            {
                failureReason = "本次请求已到时限：还没开始连接（目标 " + key.host + ":" + std::to_string(key.port) + "）";
                co_return nullptr;
            }
            auto socket = co_await connectWithDeadline(loop, dialHost, dialPort, *connectBudget, failureReason);
            if (!socket)
            {
                noteProxyDialFailure(failureReason, dialHost, dialPort, proxyRoute.has_value());
                co_return nullptr;
            }

            // https 代理（TLS 连到代理自身）：连上之后先做外层 TLS 握手，再在其上说话；
            // 请求形状不变（绝对形式 + Proxy-Authorization 都在 TLS 里走），凭据不再明文过网
            if (proxyRoute.has_value() && proxyRoute->isTls)
            {
                std::optional<std::unique_ptr<Core::TlsSocket>> tlsSocket =
                        co_await openTlsToProxy(std::move(*socket), loop, *proxyRoute, clientTls, startedAt, requestTimeout, failureReason);
                if (!tlsSocket.has_value())
                {
                    co_return nullptr;
                }
                co_return HttpOutboundConnection::forTlsToProxy(key, std::move(*tlsSocket));
            }
            co_return HttpOutboundConnection::forPlain(key, TcpStream(std::move(*socket)));
        }

        /**
         * @brief 建一条 TLS 连接：带时限连上 → 建 SSL → 设 SNI 与主机名校验 → 带 ALPN 握手
         * @details 握手成功之前这条连接还不属于池：它可能停在「对端不说话」上，所以连接与握手两段各自
         *          受剩余时限约束，超时就把 SSL 与底层描述符一起丢掉（原实现是这一段一路管到响应收完，
         *          这里只是把它拆成「连接」「握手」「交换」三段，各拿剩余的预算）。
         * @param loop 所属事件循环
         * @param u 已拆开的 URL
         * @param startedAt 本次请求的开始时刻
         * @param requestTimeout 整体时限
         * @param failureReason 输出：断在哪一段（解析地址 / 连接 / CONNECT 隧道 / TLS 那几步 / 握手）
         * @param clientTls 复用的 TLS 客户端上下文，可空
         * @param proxyRoute 非空即经代理：先拨代理、开 CONNECT 隧道，TLS 仍按目标的 SNI 与主机名校验走
         * @return std::unique_ptr<HttpOutboundConnection> 握手成功就交出连接；任一步失败返回空
         */
        Core::Task<std::unique_ptr<HttpOutboundConnection>> establishSecureConnection(Core::EventLoop &loop, const ParsedUrl &u,
                                                                                      const std::chrono::steady_clock::time_point startedAt,
                                                                                      const std::chrono::milliseconds requestTimeout, std::string &failureReason,
                                                                                      const Core::TlsContext *clientTls, const std::optional<HttpProxyRoute> &proxyRoute)
        {
            HttpOutboundEndpointKey key{u.host, u.port, true};

            // 经代理的 TLS 连接拨的是代理地址，CONNECT 之后的 TLS 与 SNI 仍按目标走（代理只搬字节）
            const auto [dialHost, dialPort]                              = dialTargetForConnection(u.host, u.port, proxyRoute);
            const std::optional<std::chrono::milliseconds> connectBudget = remainingBudget(startedAt, requestTimeout);
            if (!connectBudget.has_value())
            {
                failureReason = "本次请求已到时限：还没开始连接（主机 " + u.host + "）";
                co_return nullptr;
            }
            auto socket = co_await connectWithDeadline(loop, dialHost, dialPort, *connectBudget, failureReason);
            if (!socket)
            {
                noteProxyDialFailure(failureReason, dialHost, dialPort, proxyRoute.has_value());
                co_return nullptr;
            }
            Core::AsyncSocket sock = std::move(*socket);

            // https 代理（TLS 连到代理自身）：外层先与代理做 TLS 握手，CONNECT 在外层里发，
            // 内层 TLS 经泵跑在隧道上（TLS-in-TLS）——两个 TLS 会话各自按自己的对端校验与协商
            if (proxyRoute.has_value() && proxyRoute->isTls)
            {
                std::optional<std::unique_ptr<Core::TlsSocket>> outerTls =
                        co_await openTlsToProxy(std::move(sock), loop, *proxyRoute, clientTls, startedAt, requestTimeout, failureReason);
                if (!outerTls.has_value())
                {
                    co_return nullptr;
                }
                if (!co_await openProxyTunnelWithBudget(**outerTls, loop, u, *proxyRoute, startedAt, requestTimeout, failureReason))
                {
                    co_return nullptr;
                }
                std::optional<std::unique_ptr<HttpProxyPumpedTls>> innerTls =
                        co_await openInnerTlsThroughTunnel(std::move(*outerTls), loop, u, clientTls, startedAt, requestTimeout, failureReason);
                if (!innerTls.has_value())
                {
                    co_return nullptr;
                }
                co_return HttpOutboundConnection::forPumpedTls(key, std::move(*innerTls));
            }

            if (proxyRoute.has_value() && !co_await openProxyTunnelWithBudget(sock, loop, u, *proxyRoute, startedAt, requestTimeout, failureReason))
            {
                co_return nullptr;
            }

            std::optional<std::unique_ptr<Core::TlsSocket>> targetTls = co_await openTlsToTarget(std::move(sock), loop, u, clientTls, startedAt, requestTimeout, failureReason);
            if (!targetTls.has_value())
            {
                co_return nullptr;
            }
            co_return HttpOutboundConnection::forSecure(key, std::move(*targetTls));
        }
    } // namespace HttpClientDetail
} // namespace AsynGyanis::Net
