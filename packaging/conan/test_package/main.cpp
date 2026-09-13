// 消费方冒烟测试：像外部工程那样用这个包
//
// 判据挑的是「只有包才知道的事」：头文件路径对不对、模块间的链接关系对不对、外部依赖
// （这里用 zlib 走一次压缩）有没有被 find_dependency 带进来。协议行为由仓库自己的测试
// 覆盖，不在这里重复；也不碰事件循环——起服务器要跨线程驱动协程，那是集成测试的事，
// 放在冒烟测试里只会让它因为与本任务无关的原因变红。

#include "Net/Http/Gzip.h"
#include "Net/Http/HttpResponse.h"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

int main()
{
    constexpr std::string_view kBody = "hello from a conan consumer";

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

    std::printf("consumer_smoke: AsynGyanis::Net 可用，响应头 %zu 字节，压缩后 %zu 字节\n", head.size(), compressed->size());
    return 0;
}
