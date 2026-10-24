// 单连接 HTTP/3 负载探针：强制 h3 出站，按消息数或时长发 GET，输出一行 JSON 与 h3 链路数。
#include "Core/EventLoop/EventLoop.h"
#include "Core/Tls/TlsPolicy.h"
#include "Net/Http/Client/HttpClient.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using AsynGyanis::Core::EventLoop;
    using AsynGyanis::Core::Task;
    using AsynGyanis::Net::HttpClient;

    struct Options
    {
        std::string url{"https://127.0.0.1:18443/bench"};
        int         messages{200};
        int         durationSeconds{0};
        std::string certificateAuthority{"tests/Core/fixtures/test_ip_cert.pem"};
        long        timeoutMs{30000};
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
                options.url = next();
            } else if (flag == "--messages")
            {
                options.messages = std::stoi(next());
            } else if (flag == "--duration-seconds")
            {
                options.durationSeconds = std::stoi(next());
            } else if (flag == "--ca")
            {
                options.certificateAuthority = next();
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

    Task<> run(EventLoop &loop, HttpClient &client, const Options options, std::atomic<bool> &finished, std::atomic<bool> &succeeded)
    {
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
            auto       response       = co_await client.get(options.url, std::chrono::milliseconds{options.timeoutMs});
            if (!response || response->statusCode != 200)
            {
                ++errors;
                break;
            }
            latencies.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - messageStarted).count());
            ++sent;
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        const auto h3Links = client.idleHttp3LinkCount();
        std::printf("{\"messages\":%d,\"errors\":%d,\"p50_us\":%.3f,\"p95_us\":%.3f,\"p99_us\":%.3f,\"elapsed_ms\":%.3f,\"http3_links\":%zu}\n", sent, errors,
                    percentile(latencies, 50.0), percentile(latencies, 95.0), percentile(latencies, 99.0), elapsed, h3Links);
        std::fflush(stdout);
        succeeded = errors == 0 && sent > 0 && (options.messages == 0 || sent == options.messages) && h3Links > 0;
        finished  = true;
        co_return;
    }
} // namespace

int main(const int argumentCount, char *argumentValues[])
{
    const Options options = parseOptions(argumentCount, argumentValues);
    try
    {
        EventLoop                   loop;
        AsynGyanis::Core::TlsPolicy policy;
        policy.certificateAuthorityFile = options.certificateAuthority;
        HttpClient client(loop, {}, policy);
        client.setHttp3Enabled(true);
        std::atomic<bool> finished{false};
        std::atomic<bool> succeeded{false};
        auto              task = run(loop, client, options, finished, succeeded);
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
    } catch (const std::exception &exception)
    {
        std::fprintf(stderr, "HTTP/3 负载探针启动失败：%s\n", exception.what());
        return 1;
    }
}
