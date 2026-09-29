#include "Net/Http/HttpServerAssembly.h"

#include "Net/Http/HttpServer.h"
#include "Net/Http/HttpsServer.h"
#include "Net/Http/Middleware.h"
#include "Net/Tcp/PerIpConnectionLimiter.h"

#include <format>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 两台服务器的装配共用同一段逻辑：它们都从 TcpServer 派生且 setter 同名同义
         * @tparam ServerType HttpServer 或 HttpsServer
         */
        template<typename ServerType>
        std::expected<void, std::string> applyOnto(ServerType &server, const HttpServerConfiguration &configuration, const HttpServerAssemblyContext &context)
        {
            // 共享限额器是多台的共用对象，它的上限在构造时就定死了；配置里那个标量只对「本台新建一份」
            // 才有意义。两处都给又不相等时，静默挑一边就是「配置写了 16、实际跑的是 64」这类看不出后果的错
            if (context.sharedPerIpLimiter != nullptr && configuration.maximumConnectionsPerIp > 0 &&
                context.sharedPerIpLimiter->maximumConnectionsPerIp() != configuration.maximumConnectionsPerIp)
            {
                return std::unexpected(std::format("装配冲突：传入的共享限额器上限是 {}，而配置里的 maximum_connections_per_ip 是 {}。"
                                                   "多条通道共用一份限额器时，请只按那一份配置（把标量设为 0 或改成同一个数）",
                                                   context.sharedPerIpLimiter->maximumConnectionsPerIp(), configuration.maximumConnectionsPerIp));
            }

            server.setLimits(configuration.limits);
            server.setParserLimits(configuration.parserLimits);
            server.setMaxConnections(configuration.maximumConnections);

            // 传进来的共享对象优先；没传而配置里有标量时本台建一份（0 表示不设这道闸门，保持不动）
            if (context.sharedPerIpLimiter != nullptr)
            {
                server.setPerIpConnectionLimiter(context.sharedPerIpLimiter);
            } else if (configuration.maximumConnectionsPerIp > 0)
            {
                server.setPerIpConnectionLimiter(std::make_shared<PerIpConnectionLimiter>(configuration.maximumConnectionsPerIp));
            }

            // 限流桶同理：多台共用一份时由调用方传入，否则本台按配置建一份
            if (context.sharedRateLimitBucket != nullptr)
            {
                server.router().addMiddleware(tokenBucketRateLimiterMiddleware(context.sharedRateLimitBucket));
            } else if (configuration.requestsPerSecond > 0.0)
            {
                server.router().addMiddleware(
                        tokenBucketRateLimiterMiddleware(std::make_shared<TokenBucket>(configuration.requestsPerSecond, configuration.rateLimitBurstCapacity)));
            }

            // 运维面三件套同开：只开其一会让「抓不到数」与「以为没暴露」互相伪装
            if (configuration.exposeMetrics)
            {
                // 令牌闸门只拦 /metrics 与 /debug/loops：那两个读得到连接数、速率与每条循环的状态，
                // 而 /healthz 要能被编排器无凭据访问（正文固定、不含业务数据）。
                // 刻意不把它做成「所有路由都要令牌」：那会让业务侧自己注册的公开端点也一起被挡
                if (!configuration.opsBearerToken.empty())
                {
                    server.router().addMiddleware(opsAccessMiddleware(OpsAccessOptions{.bearerToken = configuration.opsBearerToken}));
                }
                server.enableMetricsEndpoint();
                server.enableHealthEndpoint();
                server.enableLoopDiagnosticsEndpoint();
            }
            return {};
        }
    } // namespace

    std::expected<void, std::string> applyHttpServerConfiguration(HttpServer &server, const HttpServerConfiguration &configuration, const HttpServerAssemblyContext &context)
    {
        return applyOnto(server, configuration, context);
    }

    std::expected<void, std::string> applyHttpServerConfiguration(HttpsServer &server, const HttpServerConfiguration &configuration, const HttpServerAssemblyContext &context)
    {
        return applyOnto(server, configuration, context);
    }

} // namespace AsynGyanis::Net
