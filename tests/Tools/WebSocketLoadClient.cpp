// 单连接 WebSocket 负载探针：完成一次升级，按消息数或时长发文本并等回显，输出一行 JSON。
#include "Core/EventLoop/EventLoop.h"
#include "Net/WebSocket/WebSocketClient.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using AsynGyanis::Core::EventLoop;
    using AsynGyanis::Core::Task;
    using AsynGyanis::Net::WebSocketClient;
    using AsynGyanis::Net::WebSocketOpCode;

    struct Options
    {
        std::string   host{"127.0.0.1"};
        std::uint16_t port{18080};
        std::string   path{"/ws"};
        int           messages{200};
        int           durationSeconds{0};
        std::string   payload{"acceptance"};
        long          timeoutMs{30000};
    };

    Options parseOptions(const int argumentCount, char *argumentValues[])
    {
        Options options;
        for (int index = 1; index < argumentCount; ++index)
        {
            const std::string flag{argumentValues[index]};
            const auto        next = [&]
            {
                if (index + 1 >= argumentCount)
                {
                    std::fprintf(stderr, "参数 %s 缺少取值\n", flag.c_str());
                    std::exit(2);
                }
                return std::string{argumentValues[++index]};
            };
            if (flag == "--url")
            {
                const std::string url = next();
                const std::string prefix{"ws://"};
                if (url.rfind(prefix, 0) != 0)
                {
                    std::fprintf(stderr, "只支持 ws:// URL：%s\n", url.c_str());
                    std::exit(2);
                }
                const std::size_t portBegin = url.find(':', prefix.size());
                const std::size_t pathBegin = url.find('/', portBegin == std::string::npos ? prefix.size() : portBegin);
                if (portBegin == std::string::npos)
                {
                    std::fprintf(stderr, "URL 缺少端口：%s\n", url.c_str());
                    std::exit(2);
                }
                options.host               = url.substr(prefix.size(), portBegin - prefix.size());
                const std::string portText = url.substr(portBegin + 1, pathBegin == std::string::npos ? std::string::npos : pathBegin - portBegin - 1);
                options.port               = static_cast<std::uint16_t>(std::stoi(portText));
                options.path               = pathBegin == std::string::npos ? "/" : url.substr(pathBegin);
            } else if (flag == "--messages")
            {
                options.messages = std::stoi(next());
            } else if (flag == "--duration-seconds")
            {
                options.durationSeconds = std::stoi(next());
            } else if (flag == "--payload")
            {
                options.payload = next();
            } else if (flag == "--timeout-ms")
            {
                options.timeoutMs = std::stol(next());
            } else
            {
                std::fprintf(stderr, "不认识的参数：%s\n", flag.c_str());
                std::exit(2);
            }
        }
        if (options.messages == 0 && options.durationSeconds == 0)
        {
            std::fprintf(stderr, "--messages 与 --duration-seconds 至少要有一个非 0\n");
            std::exit(2);
        }
        return options;
    }

    double percentile(std::vector<double> values, const double percent)
    {
        if (values.empty())
        {
            return 0.0;
        }
        std::sort(values.begin(), values.end());
        const std::size_t index = std::min(values.size() - 1, static_cast<std::size_t>(percent / 100.0 * static_cast<double>(values.size())));
        return values[index];
    }

    Task<> run(EventLoop &loop, const Options options, std::atomic<bool> &finished, std::atomic<bool> &succeeded)
    {
        WebSocketClient::Configuration configuration;
        configuration.hostName         = options.host;
        configuration.port             = options.port;
        configuration.requestTarget    = options.path;
        configuration.handshakeTimeout = std::chrono::milliseconds{options.timeoutMs};
        auto connected                 = co_await WebSocketClient::connect(loop, configuration);
        if (!connected)
        {
            std::printf("{\"messages\":0,\"errors\":1,\"p50_us\":0,\"p95_us\":0,\"p99_us\":0,\"elapsed_ms\":0}\n");
            std::fflush(stdout);
            finished = true;
            co_return;
        }
        auto                client = std::move(*connected);
        std::vector<double> latencies;
        int                 errors   = 0;
        int                 sent     = 0;
        const auto          started  = std::chrono::steady_clock::now();
        const auto          deadline = options.durationSeconds > 0 ? started + std::chrono::seconds{options.durationSeconds}
                                                                   : started + std::chrono::milliseconds{options.timeoutMs * static_cast<long long>(options.messages) + 30000};
        for (;;)
        {
            if (options.messages > 0 && sent >= options.messages)
            {
                break;
            }
            if (options.durationSeconds > 0 && std::chrono::steady_clock::now() >= deadline)
            {
                break;
            }
            const auto messageStarted = std::chrono::steady_clock::now();
            if (!co_await client->sendText(options.payload))
            {
                ++errors;
                break;
            }
            const auto reply = co_await client->receiveFor(std::chrono::milliseconds{options.timeoutMs});
            if (!reply || reply->opCode == WebSocketOpCode::Close || reply->payload != options.payload)
            {
                ++errors;
                break;
            }
            latencies.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - messageStarted).count());
            ++sent;
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        static_cast<void>(co_await client->close());
        std::printf("{\"messages\":%d,\"errors\":%d,\"p50_us\":%.3f,\"p95_us\":%.3f,\"p99_us\":%.3f,\"elapsed_ms\":%.3f}\n", sent, errors, percentile(latencies, 50.0),
                    percentile(latencies, 95.0), percentile(latencies, 99.0), elapsed);
        std::fflush(stdout);
        succeeded = errors == 0 && sent > 0 && (options.messages == 0 || sent == options.messages);
        finished  = true;
        co_return;
    }
} // namespace

int main(const int argumentCount, char *argumentValues[])
{
    const Options     options = parseOptions(argumentCount, argumentValues);
    EventLoop         loop;
    std::atomic<bool> finished{false};
    std::atomic<bool> succeeded{false};
    auto              task = run(loop, options, finished, succeeded);
    loop.scheduler().schedule(task.handle());
    std::thread loopThread{[&loop] { loop.run(); }};
    const auto  deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{options.timeoutMs * 2 + options.durationSeconds * 1000LL};
    while (!finished.load() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    loop.stop();
    loopThread.join();
    return succeeded.load() ? 0 : 1;
}
