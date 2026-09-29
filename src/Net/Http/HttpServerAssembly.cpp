#include "Net/Http/HttpServerAssembly.h"

#include "Net/Http/HttpServer.h"
#include "Net/Http/HttpsServer.h"
#include "Net/Http/Middleware.h"
#include "Net/Tcp/PerIpConnectionLimiter.h"

#include <format>
#include <utility>

namespace AsynGyanis::Net
{
    std::size_t perProcessShare(const std::size_t wholeMachineValue, const std::size_t workerProcessCount) noexcept
    {
        // 0 是「显式不限」，不参与摊分；count<=1 时原样返回，避免为单进程做一次无意义的除法
        if (wholeMachineValue == 0 || workerProcessCount <= 1)
        {
            return wholeMachineValue;
        }
        // 向上取整：宁可每台多几个名额，也不要「整机 100 摊成 4×25=100 但被向下取整吃掉余数」，
        // 那种情况下配置写的整机上限永远达不到，而差多少没人去算
        return (wholeMachineValue + workerProcessCount - 1) / workerProcessCount;
    }

    namespace
    {
        /**
         * @brief 两台服务器的装配共用同一段逻辑：它们都从 TcpServer 派生且 setter 同名同义
         * @tparam ServerType HttpServer 或 HttpsServer
         */
        template<typename ServerType>
        std::expected<void, std::string> applyOnto(ServerType &server, const HttpServerConfiguration &configuration, const HttpServerAssemblyContext &context)
        {
            // 摊到 0 个进程没有意义，而这里接下来要拿它做除数：当场拒，不悄悄当成「不摊」
            if (context.workerProcessCount == 0)
            {
                return std::unexpected("装配冲突：workerProcessCount 是 0；单进程请填 1");
            }
            // 每个进程只看得见自己这份账，所以配置里的整机上限要摊下来才真是那个数——否则起 N 个进程
            // 就等于放行 N 倍，而配置文件上写的仍是整机的那个数
            const std::size_t perProcessMaximumConnections  = perProcessShare(configuration.maximumConnections, context.workerProcessCount);
            const std::size_t perProcessMaximumPerIp        = perProcessShare(configuration.maximumConnectionsPerIp, context.workerProcessCount);

            // 共享限额器是多台的共用对象，它的上限在构造时就定死了；配置里那个标量只对「本台新建一份」
            // 才有意义，且要多进程时是摊过的一份。两处都给又不相等时，静默挑一边就是
            // 「配置写了 16、实际跑的是 64」这类看不出后果的错
            if (context.sharedPerIpLimiter != nullptr && perProcessMaximumPerIp > 0 &&
                context.sharedPerIpLimiter->maximumConnectionsPerIp() != perProcessMaximumPerIp)
            {
                return std::unexpected(std::format("装配冲突：传入的共享限额器上限是 {}，而配置摊到本进程后应是 {}"
                                                   "（maximum_connections_per_ip={} 摊给 {} 个进程）。"
                                                   "多条通道共用一份限额器时，请让那一份与整机配置对得上（或对不上时把标量设为 0）",
                                                   context.sharedPerIpLimiter->maximumConnectionsPerIp(),
                                                   perProcessMaximumPerIp,
                                                   configuration.maximumConnectionsPerIp,
                                                   context.workerProcessCount));
            }

            server.setLimits(configuration.limits);
            server.setParserLimits(configuration.parserLimits);
            server.setMaxConnections(perProcessMaximumConnections);

            // 传进来的共享对象优先；没传而配置里有标量时本台建一份（0 表示不设这道闸门，保持不动）
            if (context.sharedPerIpLimiter != nullptr)
            {
                server.setPerIpConnectionLimiter(context.sharedPerIpLimiter);
            } else if (perProcessMaximumPerIp > 0)
            {
                server.setPerIpConnectionLimiter(std::make_shared<PerIpConnectionLimiter>(perProcessMaximumPerIp));
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
