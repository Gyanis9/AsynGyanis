// 消费方冒烟测试（vcpkg 侧）：像外部工程那样用这个包
//
// 与 packaging/conan/test_package 同一判据口径：只量「只有包才知道的事」——头文件路径、
// 模块间的链接关系、外部依赖有没有被 find_dependency 带进来。协议行为由仓库自己的测试覆盖，
// 不在这里重复；也不起服务器（那要跨线程驱动协程，与本任务无关的抖动都会让它变红）。
// 这里额外钉一条 vcpkg 侧才有的判据：出站客户端（连接池 + h2）这三个观测计数随包可用——
// 它是最后加进公共面的一批符号，最容易被导出清单漏掉。

#include "Core/EventLoop/EventLoop.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/Gzip.h"
#include "Net/Http/HttpResponse.h"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

int main()
{
    constexpr std::string_view kBody = "hello from a vcpkg consumer";

    AsynGyanis::Net::HttpResponse response;
    response.setStatus(200);
    response.setBody(std::string(kBody));
    const std::string head = response.serializeHead();
    if (!head.starts_with("HTTP/1.1 200"))
    {
        std::printf("consumer_smoke: 响应头不对：%s\n", head.c_str());
        return 1;
    }

    // 走一次 zlib：这条链路能通，说明包把外部依赖一并带给了消费方
    const std::optional<std::string> compressed = AsynGyanis::Net::gzipCompress(kBody);
    if (!compressed.has_value() || compressed->empty())
    {
        std::printf("consumer_smoke: gzip 压缩不可用（外部依赖没链上）\n");
        return 1;
    }

    // 出站客户端：空池上三个计数都该是 0，closeIdleConnections() 该能空跑一次
    AsynGyanis::Core::EventLoop loop;
    AsynGyanis::Net::HttpClient client(loop);
    if (client.idleConnectionCount() != 0U || client.idleHttp2ConnectionCount() != 0U
        || client.http2MaximumInFlightStreamCount() != 0U)
    {
        std::printf("consumer_smoke: 新建客户端的连接池不是空的\n");
        return 1;
    }
    client.closeIdleConnections();

    std::printf("consumer_smoke: AsynGyanis::Net 可用，响应头 %zu 字节，压缩后 %zu 字节\n", head.size(), compressed->size());
    return 0;
}
