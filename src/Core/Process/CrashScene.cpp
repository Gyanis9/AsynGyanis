#include "Core/Process/CrashScene.h"

#include "Base/Exception/StackTrace.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/Platform.h"
#include "Platform/System/ProcessInfo.h"

#include <array>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <string>
#include <string_view>
#include <thread>

#if ASYN_PLATFORM_WIN32
#include <io.h> // ::_write 的声明在这里（现场写 stderr 走 CRT 低层写，见 writeSceneText）
#include <windows.h>
#else
#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace AsynGyanis::Core
{
    namespace
    {
        std::atomic<bool>      g_isInstalled{false};
        std::terminate_handler g_previousTerminateHandler{nullptr};

        /// 当前进程号（信号路径直取，不分配）
        std::uint64_t currentProcessIdForSignalPath() noexcept
        {
#if ASYN_PLATFORM_WIN32
            return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
            return static_cast<std::uint64_t>(::getpid());
#endif
        }

        /// 当前线程指纹（信号路径直取，不分配）：fork 之前取好传给完整现场，别写成子进程那条线程的
        std::uint64_t currentThreadFingerprintForSignalPath() noexcept
        {
#if ASYN_PLATFORM_WIN32
            return static_cast<std::uint64_t>(::GetCurrentThreadId());
#else
            return static_cast<std::uint64_t>(::pthread_self());
#endif
        }

#if !ASYN_PLATFORM_WIN32
        /// 备用信号栈（每条线程各自一份）：爆栈引发的 SIGSEGV 只有在它上面，处理器才跑得起来
        constexpr std::size_t                                            kAlternativeSignalStackBytes = 128U * 1024U;
        thread_local std::array<std::byte, kAlternativeSignalStackBytes> g_alternativeSignalStack{};
#endif

        /// stderr 的描述符号码：现场写这里而不是日志器——崩溃时刻日志器可能正持锁或有未刷出的缓冲
        constexpr int kStandardErrorDescriptor = 2;

        /// 用一次 write 把整段文本送出去；写不全不重试——处理器里重试的代价比现场少几行大得多
        void writeSceneText(const std::string_view text) noexcept
        {
            if (text.empty())
            {
                return;
            }
#if ASYN_PLATFORM_WIN32
            // stderr 在 Windows 上是 CRT 的 fd（最终落 WriteFile），不是 socket：FileDescriptor::write
            // 走 ::send 会以 WSAENOTSOCK 失败，现场一个字都出不去——这里走 CRT 的低层 _write
            [[maybe_unused]] const auto writtenCount = ::_write(kStandardErrorDescriptor, text.data(), static_cast<unsigned int>(text.size()));
#else
            [[maybe_unused]] const auto writtenCount = Platform::FileDescriptor::write(kStandardErrorDescriptor, text.data(), text.size());
#endif
        }

        /**
         * @brief 最小现场：静态文本 + 手工转写的进程号与线程指纹，只经一次 write() 落盘
         * @details 崩在分配器持锁处时，任何 std::string / std::format / 符号解析都会在同线程自锁；
         *          这一条必须先于一切进得去、写得完。会分配的那部分（完整现场）由调用方移到 fork
         *          出来的子进程里做，本函数自身不做任何重活
         * @param causeText 致因文本（静态字面量或既有字符串，逐字节拷进栈上缓冲）
         * @param threadFingerprint 发生崩溃的那条线程的指纹（fork 之前由调用方取好）
         */
        void reportMinimalScene(const std::string_view causeText, const std::uint64_t threadFingerprint) noexcept
        {
            char        buffer[320];
            std::size_t length = 0;
            const auto  append = [&buffer, &length](const std::string_view text)
            {
                for (const char character: text)
                {
                    if (length + 1 >= sizeof(buffer))
                    {
                        return;
                    }
                    buffer[length++] = character;
                }
            };
            const auto appendUnsigned = [&buffer, &length](std::uint64_t value)
            {
                // 手工十进制转写：snprintf 属 stdio、不在异步信号安全名单里
                char        digits[24];
                std::size_t digitCount = 0;
                do
                {
                    digits[digitCount++] = static_cast<char>('0' + value % 10U);
                    value /= 10U;
                } while (value != 0U && digitCount < sizeof(digits));
                while (digitCount > 0 && length + 1 < sizeof(buffer))
                {
                    buffer[length++] = digits[--digitCount];
                }
            };

            append("=== 崩溃现场（最小）===\n致因：");
            append(causeText);
            append("，进程 ");
            appendUnsigned(currentProcessIdForSignalPath());
            append("，线程指纹 ");
            appendUnsigned(threadFingerprint);
            append("（完整现场随后尽力补写）\n");
            writeSceneText(std::string_view{buffer, length});
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
        void reportCrashScene(const std::string_view causeText, const std::uint64_t threadFingerprint) noexcept
        {
            try
            {
                std::string scene;
                scene.reserve(4096);
                scene += "=== 崩溃现场（best-effort）===\n";
                scene += "进程 ";
                scene += std::to_string(Platform::ProcessInfo::currentProcessId());
                scene += "，线程指纹 ";
                scene += std::format("{:016x}", threadFingerprint);
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

#if ASYN_PLATFORM_WIN32
        /// Windows 没有 SIGBUS；其余同名信号由 CRT 把相应硬件异常路由进来
        constexpr int kCrashSignals[] = {SIGSEGV, SIGABRT, SIGFPE, SIGILL};
#else
        constexpr int kCrashSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
#endif

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
            const std::uint64_t threadFingerprint = currentThreadFingerprintForSignalPath();

            // ① 最小现场：静态文本 + 手工转写，只经 write()——崩在分配器持锁处时这一条仍写得完
            reportMinimalScene(causeText, threadFingerprint);

#if !ASYN_PLATFORM_WIN32
            // ② 完整现场（格式化与调用栈捕获都会分配）移进 fork 出来的子进程：子进程若因继承的锁
            //    自锁挂住，只挂它自己（alarm 兜底收走），父进程照常走到下面「恢复默认并重发」——
            //    「死得正确」（WIFSIGNALED 与 coredump）不再依赖现场组装能不能完成
            if (const pid_t sceneWriter = ::fork(); sceneWriter == 0)
            {
                // 子进程里把整组致命信号恢复默认：完整现场若自身再撞信号（例如在副本栈上再溢），
                // 安静地死，不能再进一遍处理器（那会再 fork 一层，链式繁殖）
                for (const int crashSignal: kCrashSignals)
                {
                    std::signal(crashSignal, SIG_DFL);
                }
                ::alarm(2);
                reportCrashScene(causeText, threadFingerprint);
                ::_exit(0);
            }
#else
            // Windows 没有 fork：完整现场在进程内尽力写（最小现场已在上面落过，最坏也只是缺后半段）
            reportCrashScene(causeText, threadFingerprint);
#endif

            // 恢复默认处置后重发。重发后面不能接任何兜底 exit：信号在处理器执行期间会被加入
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
            // 与信号路径同一取舍：最小现场先落（terminate 可能正逢分配器或日志器的坏状态），
            // 完整现场随后在进程内补
            const std::uint64_t threadFingerprint = currentThreadFingerprintForSignalPath();
            reportMinimalScene(causeText, threadFingerprint);
            reportCrashScene(causeText, threadFingerprint);

            if (g_previousTerminateHandler != nullptr)
            {
                // 上一个处理器（默认就是 abort）会再发 SIGABRT：先把我们自己的 SIGABRT 处置恢复成
                // 默认，免得死前又走进本处理器把现场写第二遍
                std::signal(SIGABRT, SIG_DFL);
                g_previousTerminateHandler();
            }
            std::abort();
        }
    } // namespace

    bool CrashScene::install() noexcept
    {
        // altstack 按线程装：信号处置是进程级的，但备用栈是每条线程各自的资源。本次调用先把
        // 「调用线程」的备用栈挂上（重复踩到这里也顺手补齐），爆栈引发的 SIGSEGV 才有落脚处
#if !ASYN_PLATFORM_WIN32
        stack_t alternativeStack{};
        alternativeStack.ss_sp    = g_alternativeSignalStack.data();
        alternativeStack.ss_size  = g_alternativeSignalStack.size();
        alternativeStack.ss_flags = 0;
        // 挂不上就退回普通栈：SA_ONSTACK 在没配栈的线程上等价于不加该标志，不死磕
        [[maybe_unused]] const int stackResult = ::sigaltstack(&alternativeStack, nullptr);
#endif

        if (g_isInstalled.exchange(true, std::memory_order_acq_rel))
        {
            return false;
        }

#if ASYN_PLATFORM_WIN32
        for (const int crashSignal: kCrashSignals)
        {
            std::signal(crashSignal, &handleFatalSignal);
        }
#else
        struct sigaction crashAction{};
        crashAction.sa_handler = &handleFatalSignal;
        crashAction.sa_flags   = SA_ONSTACK;
        ::sigemptyset(&crashAction.sa_mask);
        for (const int crashSignal: kCrashSignals)
        {
            [[maybe_unused]] const int actionResult = ::sigaction(crashSignal, &crashAction, nullptr);
        }
#endif
        g_previousTerminateHandler = std::set_terminate(&handleTerminate);
        return true;
    }

    bool CrashScene::isInstalled() noexcept
    {
        return g_isInstalled.load(std::memory_order_acquire);
    }
} // namespace AsynGyanis::Core
