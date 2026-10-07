// 消费方冒烟测试：像外部工程那样用这个包
//
// 判据挑的是「只有包才知道的事」：头文件路径对不对、模块间的链接关系对不对、外部依赖
// （这里用 zlib 走一次压缩）有没有被 find_dependency 带进来。协议行为由仓库自己的测试
// 覆盖，不在这里重复；也不碰事件循环——起服务器要跨线程驱动协程，那是集成测试的事，
// 放在冒烟测试里只会让它因为与本任务无关的原因变红。

#include "Base/Exception/StackTrace.h"
#include "Base/Log/Formatters/DefaultFormatter.h"
#include "Base/Log/Sinks/RollingFileSink.h"
#include "Database/MySql/MySqlConnection.h"
#include "Database/Redis/RedisConnection.h"
#include "Net/Http/Gzip.h"
#include "Net/Http/HttpResponse.h"
#include "Platform/IO/NetworkInterface.h"

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

    // 平台模块的这条入口在 Windows SDK 里由 iphlpapi.lib 实现（if_nametoindex），在 Linux 侧在 libc。
    // 引用它不是为了测出什么取值——静态库要把实现该符号的那个目标文件拉进最终可执行体，
    // 少声明一个系统库就是在这里链接期报未解析外部符号；少了这一步，「包漏了 iphlpapi」这种缺陷
    // 在本冒烟里根本走不到链接器
    const unsigned interfaceIndex = AsynGyanis::Platform::interfaceIndexOfName("lo");
    static_cast<void>(interfaceIndex); // 有没有这块网卡随机器而定，判据是「链得上、跑得掉」

    // 调用栈那条能力的形状：包按 cpp_info 交出的宏，必须与库体里编进去的那一份是同一个答案。
    // 宏说有栈而库给的是空栈，就是两边按 #if 分了叉（CapturedStackTrace 在两种形状下是不同类型）
#if defined(ASYN_HAS_STACKTRACE)
    if (AsynGyanis::Base::captureStackTrace(0, 8).empty())
    {
        std::printf("consumer_smoke: 宏声明有调用栈，库却交出空栈（包与库体的形状不一致）\n");
        return 1;
    }
#endif

    // 走一遍刚从安装清单里收紧的那几处的「上层头」：RollingFileSink.h / DefaultFormatter.h 背后
    // 是 Detail/RollingPeriod.h 与 Detail/PlainTextLogLine.h，MySql/Redis 那两个公开头的兄弟
    // （MySqlValueConversion.h、RedisReplyText.h）已经不再随包发出。仓库外的工程能把这几份头
    // 包含干净，才说明「排除的是实现侧的头」而不是「切断了使用方要走的链」
    volatile auto formatterProbe = &AsynGyanis::Base::DefaultFormatter::format;
    static_cast<void>(formatterProbe);

    std::printf("consumer_smoke: AsynGyanis::Net 可用，响应头 %zu 字节，压缩后 %zu 字节\n", head.size(), compressed->size());
    return 0;
}
