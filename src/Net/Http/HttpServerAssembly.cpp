#include "Net/Http/HttpServerAssembly.h"

#include "Net/Http/HttpMemoryBudget.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/HttpsServer.h"
#include "Net/Http/Middleware.h"
#include "Net/Tcp/PerIpConnectionLimiter.h"

#include <algorithm>
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

    PerProcessRateLimit perProcessRateLimit(const double wholeMachineRequestsPerSecond, const double wholeMachineBurstCapacity, const std::size_t workerProcessCount) noexcept
    {
        if (wholeMachineRequestsPerSecond <= 0.0)
        {
            return PerProcessRateLimit{};
        }
        if (workerProcessCount <= 1)
        {
            return PerProcessRateLimit{.requestsPerSecond = wholeMachineRequestsPerSecond, .burstCapacity = wholeMachineBurstCapacity};
        }
        const double processCount = static_cast<double>(workerProcessCount);
        // 容量兜在 1.0：整机突发量小于进程数时（例如桶容量 2 摊给 4 个进程），整除会给出一个
        // 攒不满一枚令牌的桶——TokenBucket 的构造直接拒绝容量小于 1，宁可每台各留一个突发名额
        return PerProcessRateLimit{.requestsPerSecond = wholeMachineRequestsPerSecond / processCount, .burstCapacity = std::max(1.0, wholeMachineBurstCapacity / processCount)};
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
            // 每进程台数同一条判据：一台进程里跑 L 条监听器时，每条各持一份自己的计数，
            // 只按进程数摊就等于放行 L 倍（`maximum_connections` 写着 4096、四个循环线程实际收 16384，
            // 而配置文件看着仍是 4096）。0 是「一台都不接」，与上面的 0 进程同一类用法错误
            if (context.listenersPerProcess == 0)
            {
                return std::unexpected("装配冲突：listenersPerProcess 是 0；本进程只有一条监听器请填 1");
            }
            // 两个分母要分清，否则「共用一份账」的那几项会被摊成两份以下：
            //   · 每台自己计数的（连接数；以及本台自建一份时的限额器/限流桶/在途预算）——进程数 × 每进程台数
            //   · 调用方传进来的共享对象（一个进程里多台共用一份）——只按进程数摊
            const std::size_t perListenerDenominator        = context.workerProcessCount * context.listenersPerProcess;
            const std::size_t perListenerMaximumConnections = perProcessShare(configuration.maximumConnections, perListenerDenominator);
            const std::size_t perProcessMaximumPerIp        = perProcessShare(configuration.maximumConnectionsPerIp, context.workerProcessCount);

            // 共享限额器是多台的共用对象，它的上限在构造时就定死了；配置里那个标量只对「本台新建一份」
            // 才有意义，且要多进程时是摊过的一份。两处都给又不相等时，静默挑一边就是
            // 「配置写了 16、实际跑的是 64」这类看不出后果的错
            if (context.sharedPerIpLimiter != nullptr && perProcessMaximumPerIp > 0 && context.sharedPerIpLimiter->maximumConnectionsPerIp() != perProcessMaximumPerIp)
            {
                return std::unexpected(std::format("装配冲突：传入的共享限额器上限是 {}，而配置摊到本进程后应是 {}"
                                                   "（maximum_connections_per_ip={} 摊给 {} 个进程）。"
                                                   "多条通道共用一份限额器时，请让那一份与整机配置对得上（或对不上时把标量设为 0）",
                                                   context.sharedPerIpLimiter->maximumConnectionsPerIp(), perProcessMaximumPerIp, configuration.maximumConnectionsPerIp,
                                                   context.workerProcessCount));
            }

            server.setLimits(configuration.limits);
            server.setParserLimits(configuration.parserLimits);
            server.setMaxConnections(perListenerMaximumConnections);

            // 传进来的共享对象优先；没传而配置里有标量时本台建一份（0 表示不设这道闸门，保持不动）
            if (context.sharedPerIpLimiter != nullptr)
            {
                server.setPerIpConnectionLimiter(context.sharedPerIpLimiter);
            } else if (configuration.maximumConnectionsPerIp > 0)
            {
                // 本台自建的那一份只有这一台看得见，因此按每台摊；共享对象那一条路保持进程口径不变
                server.setPerIpConnectionLimiter(std::make_shared<PerIpConnectionLimiter>(perProcessShare(configuration.maximumConnectionsPerIp, perListenerDenominator)));
            }

            // 限流桶同理：多台共用一份时由调用方传入，否则本台按配置建一份（0 表示不设这道闸门，保持不动）
            // 传入的那一份必须与摊分结果一致——桶的速率在构造时就定死，配置写着整机 100 而桶跑的是
            // 100/进程，四个进程就放行 400，与连接数那条被拒的偏差长得一模一样，不该只有一处出声
            const PerProcessRateLimit rateShare = perProcessRateLimit(configuration.requestsPerSecond, configuration.rateLimitBurstCapacity, context.workerProcessCount);
            if (context.sharedRateLimitBucket != nullptr)
            {
                if (rateShare.requestsPerSecond > 0.0 &&
                    (context.sharedRateLimitBucket->tokensPerSecond() != rateShare.requestsPerSecond || context.sharedRateLimitBucket->burstCapacity() != rateShare.burstCapacity))
                {
                    return std::unexpected(std::format("装配冲突：传入的共享限流桶是 {} 请求/s（桶容量 {}），而配置摊到本进程后应是 {} 请求/s（桶容量 {}）"
                                                       "（rate_limit.requests_per_second={} 摊给 {} 个进程）。"
                                                       "多条通道共用一个桶时，请让那一个与整机配置对得上（或对不上时把速率设为 0）",
                                                       context.sharedRateLimitBucket->tokensPerSecond(), context.sharedRateLimitBucket->burstCapacity(),
                                                       rateShare.requestsPerSecond, rateShare.burstCapacity, configuration.requestsPerSecond, context.workerProcessCount));
                }
                server.router().addMiddleware(tokenBucketRateLimiterMiddleware(context.sharedRateLimitBucket));
            } else if (configuration.requestsPerSecond > 0.0)
            {
                // 桶是本台自己 new 的，一个进程里几台就各有一份，因此按「进程 × 台数」摊；
                // 上面那条共享对象的比对仍按进程口径（一台进程里多台共用一个桶）
                const PerProcessRateLimit perListenerRateShare = perProcessRateLimit(configuration.requestsPerSecond, configuration.rateLimitBurstCapacity, perListenerDenominator);
                server.router().addMiddleware(
                        tokenBucketRateLimiterMiddleware(std::make_shared<TokenBucket>(perListenerRateShare.requestsPerSecond, perListenerRateShare.burstCapacity)));
            }

            // 在途正文预算是跨连接的一份账，且账目只在进程内可见：整机口径同样要摊到本进程。
            // 传进来的那一份必须与摊分结果一致——预算的上限在构造时就定死了，配置写 512 MiB
            // 而对象各持整机那份，N 个进程就放行 N×512 MiB，而配置文件看着仍是 512 MiB
            const std::size_t perProcessMemoryBudget = perProcessShare(configuration.memoryBudgetBytes, context.workerProcessCount);
            if (context.sharedMemoryBudget != nullptr)
            {
                if (perProcessMemoryBudget > 0 && context.sharedMemoryBudget->maximumTotalBytes() != perProcessMemoryBudget)
                {
                    return std::unexpected(std::format("装配冲突：传入的共享预算上限是 {} 字节，而配置摊到本进程后应是 {} 字节"
                                                       "（memory_budget_bytes={} 摊给 {} 个进程）。"
                                                       "多条通道共用一份账时，请让那一份与整机配置对得上（或对不上时把 memory_budget_bytes 设为 0）",
                                                       context.sharedMemoryBudget->maximumTotalBytes(), perProcessMemoryBudget, configuration.memoryBudgetBytes,
                                                       context.workerProcessCount));
                }
                server.setMemoryBudget(context.sharedMemoryBudget);
            } else if (configuration.memoryBudgetBytes > 0)
            {
                // 自建的那份账只覆盖这一台，按台摊；共享对象那条路仍按进程口径比对
                server.setMemoryBudget(std::make_shared<HttpMemoryBudget>(perProcessShare(configuration.memoryBudgetBytes, perListenerDenominator)));
            }

            // 运维面三件套同开：只开其一会让「抓不到数」与「以为没暴露」互相伪装。
            // metrics_port 非 0 时这里什么都不挂：端点归调用方另起的那台管理监听器（默认只听回环，
            // 多进程时每个进程一个端口）——留在业务口上就等于跟着业务口一起公开出去
            if (configuration.metricsPort == 0)
            {
                registerOperationEndpoints(server, configuration);
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
