#include "Core/Process/CrashScene.h"

#include "Base/Exception/StackTrace.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/Platform.h"
#include "Platform/System/ProcessInfo.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <format>
#include <string>
#include <string_view>
#include <thread>

namespace AsynGyanis::Core
{
    namespace
    {
        std::atomic<bool>      g_isInstalled{false};
        std::terminate_handler g_previousTerminateHandler{nullptr};

        /// stderr 的描述符号码：现场写这里而不是日志器——崩溃时刻日志器可能正持锁或有未刷出的缓冲
        constexpr int kStandardErrorDescriptor = 2;

        /// 用一次 write 把整段文本送出去；写不全不重试——处理器里重试的代价比现场少几行大得多
        void writeSceneText(const std::string_view text) noexcept
        {
            if (text.empty())
            {
                return;
            }
            [[maybe_unused]] const auto writtenCount = Platform::FileDescriptor::write(kStandardErrorDescriptor, text.data(), text.size());
        }

        /// 捕获一份栈文本；捕获与符号解析都可能分配，失败即退化为空（现场照写，只是没有栈）
        std::string captureSceneStackTrace() noexcept
        {
            try
            {
                return Base::formatStackTrace(Base::captureStackTrace());
            } catch (...)
            {
                return {};
            }
        }

        /// 组装并写出一次现场；组装过程本身也可能分配，失败就退化成最小现场
        void reportCrashScene(const std::string_view causeText) noexcept
        {
            try
            {
                std::string scene;
                scene.reserve(4096);
                scene += "=== 崩溃现场（best-effort）===\n";
                scene += "进程 ";
                scene += std::to_string(Platform::ProcessInfo::currentProcessId());
                scene += "，线程指纹 ";
                scene += std::format("{:016x}", std::hash<std::thread::id>{}(std::this_thread::get_id()));
                scene += "\n致因：";
                scene += causeText;
                scene += "\n调用栈（符号解析不可用时可能为空）：\n";
                scene += captureSceneStackTrace();
                if (!scene.ends_with('\n'))
                {
                    scene += '\n';
                }
                scene += "=== 现场结束 ===\n";
                writeSceneText(scene);
            } catch (...)
            {
                writeSceneText("=== 崩溃现场：组装失败（内存或符号解析不可用）===\n");
            }
        }

        /// 致命信号的处理：写现场，再把处置恢复成默认并重发——进程必须仍以「信号致死」退出，
        /// WIFSIGNALED 与 coredump 的语义都留给默认处置
        void handleFatalSignal(const int signalNumber) noexcept
        {
            const char *causeText = "收到致命信号";
            switch (signalNumber)
            {
                case SIGSEGV:
                    causeText = "收到 SIGSEGV（无效内存访问）";
                    break;
                case SIGABRT:
                    causeText = "收到 SIGABRT（abort 或断言失败）";
                    break;
#if !ASYN_PLATFORM_WIN32
                case SIGBUS:
                    causeText = "收到 SIGBUS（总线错误）";
                    break;
#endif
                case SIGFPE:
                    causeText = "收到 SIGFPE（算术异常）";
                    break;
                case SIGILL:
                    causeText = "收到 SIGILL（非法指令）";
                    break;
                default:
                    break;
            }
            reportCrashScene(causeText);

            // 恢复默认处置后重发。**重发后面不能接任何兜底 exit**：信号在处理器执行期间会被加入
            // 屏蔽字，这一次 raise 只是把它置成挂起态，真正交付发生在处理器返回那一刻（按已经恢复
            // 的默认处置致死，coredump 语义也在）；在那之前退出会把死法换成普通退出码，
            // WIFSIGNALED 为假，上层看护会把崩溃读成「正常退出」
            std::signal(signalNumber, SIG_DFL);
            std::raise(signalNumber);
        }

        /// std::terminate 的处理：尽量取出逃逸的异常原文，写现场后交回上一个处理器（默认会 abort）
        void handleTerminate() noexcept
        {
            std::string causeText = "std::terminate（未捕获异常或异常逃出 noexcept 边界）";
            try
            {
                if (const std::exception_ptr escapingException = std::current_exception())
                {
                    std::rethrow_exception(escapingException);
                }
            } catch (const std::exception &exception)
            {
                causeText += "，逃逸的异常：";
                causeText += exception.what();
            } catch (...)
            {
                causeText += "，逃逸的异常不是 std::exception 家族";
            }
            reportCrashScene(causeText);

            if (g_previousTerminateHandler != nullptr)
            {
                // 上一个处理器（默认就是 abort）会再发 SIGABRT：先把我们自己的 SIGABRT 处置恢复成
                // 默认，免得死前又走进本处理器把现场写第二遍
                std::signal(SIGABRT, SIG_DFL);
                g_previousTerminateHandler();
            }
            std::abort();
        }

#if ASYN_PLATFORM_WIN32
        /// Windows 没有 SIGBUS；其余同名信号由 CRT 把相应硬件异常路由进来
        constexpr int kCrashSignals[] = {SIGSEGV, SIGABRT, SIGFPE, SIGILL};
#else
        constexpr int kCrashSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
#endif
    } // namespace

    bool CrashScene::install() noexcept
    {
        if (g_isInstalled.exchange(true, std::memory_order_acq_rel))
        {
            return false;
        }

        for (const int crashSignal: kCrashSignals)
        {
            std::signal(crashSignal, &handleFatalSignal);
        }
        g_previousTerminateHandler = std::set_terminate(&handleTerminate);
        return true;
    }

    bool CrashScene::isInstalled() noexcept
    {
        return g_isInstalled.load(std::memory_order_acquire);
    }
} // namespace AsynGyanis::Core
