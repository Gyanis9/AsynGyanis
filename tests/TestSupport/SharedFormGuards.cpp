#include "SharedFormGuards.h"

namespace AsynGyanis::TestSupport
{
    // 判据落在实现文件而不是头里：头里的 constexpr 值会让 MSVC 认定 GTEST_SKIP() 之后的用例体不可达
    // （C4702，本仓库 /WX 下即错误），而运行期常量不会做那个推断。
    // 五个模块里凡把 OpenSSL 编进自己镜像的那几个（Core 的 TLS 栈、Net 的 QUIC、Database 的 MySQL 驱动）
    // 一旦被本可执行体之外的方式装载，用例侧与库侧就是两份实例。
#if defined(ASYN_CORE_SHARED_LIB) || defined(ASYN_NET_SHARED_LIB) || defined(ASYN_DATABASE_SHARED_LIB)
    const bool kTlsHarnessIsCrossInstance = true;
#else
    const bool kTlsHarnessIsCrossInstance = false;
#endif
} // namespace AsynGyanis::TestSupport
