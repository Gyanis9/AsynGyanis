#include "Core/Process/CrashScene.h"

#include "Base/Exception/StackTrace.h"
#include "Platform/Platform.h"
#include "Platform/System/CrashHandler.h"
#include "Platform/System/ProcessInfo.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <string>
#include <string_view>

namespace AsynGyanis::Core
{
    namespace
    {
        std::terminate_handler g_previousTerminateHandler{nullptr};

        /**
         * @brief 致命信号对应的中文致因
         * @param signal 平台无关的信号类别
         * @return const char * 静态文本；现场路径不分配
         */
        const char *causeTextOf(const Platform::CrashSignal signal) noexcept
        {
            switch (signal)
            {
                case Platform::CrashSignal::SegmentationFault:
                    return "收到 SIGSEGV（无效内存访问）";
                case Platform::CrashSignal::Abort:
                    return "收到 SIGABRT（abort 或断言失败）";
                case Platform::CrashSignal::BusError:
                    return "收到 SIGBUS（总线错误）";
                case Platform::CrashSignal::ArithmeticFault:
                    return "收到 SIGFPE（算术异常）";
                case Platform::CrashSignal::IllegalInstruction:
                    return "收到 SIGILL（非法指令）";
                case Platform::CrashSignal::Unknown:
                    break;
            }
            return "收到致命信号";
        }

        /**
         * @brief 最小现场：静态文本 + 手工转写的进程号与线程指纹，只经一次 write() 落盘
         * @details 崩在分配器持锁处时，任何 std::string / std::format / 符号解析都会在同线程自锁；
         *          这一条必须先于一切进得去、写得完。会分配的那部分（完整现场）由 Platform 移到 fork
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
            appendUnsigned(static_cast<std::uint64_t>(Platform::ProcessInfo::currentProcessId()));
            append("，线程指纹 ");
            appendUnsigned(threadFingerprint);
            append("（完整现场随后尽力补写）\n");
            Platform::CrashHandler::writeStandardError(std::string_view{buffer, length});
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
                Platform::CrashHandler::writeStandardError(scene);
            } catch (...)
            {
                Platform::CrashHandler::writeStandardError("=== 崩溃现场：组装失败（内存或符号解析不可用）===\n");
            }
        }

        /// Platform 的最小现场回调：把信号类别翻成致因后走无分配的那一条
        void reportMinimalSignalScene(const Platform::CrashSignal signal, void *) noexcept
        {
            reportMinimalScene(causeTextOf(signal), Platform::ProcessInfo::currentThreadFingerprint());
        }

        /// Platform 的完整现场回调：Linux 上它在 fork 出的子进程里执行，可以分配
        void reportFullSignalScene(const Platform::CrashSignal signal, void *) noexcept
        {
            reportCrashScene(causeTextOf(signal), Platform::ProcessInfo::currentThreadFingerprint());
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
            const std::uint64_t threadFingerprint = Platform::ProcessInfo::currentThreadFingerprint();
            reportMinimalScene(causeText, threadFingerprint);
            reportCrashScene(causeText, threadFingerprint);

            if (g_previousTerminateHandler != nullptr)
            {
                // 上一个处理器（默认就是 abort）会再发 SIGABRT：先把我们自己的那条处置恢复成
                // 默认，免得死前又走进本处理器把现场写第二遍
                Platform::CrashHandler::restoreDefaultDisposition(Platform::CrashSignal::Abort);
                g_previousTerminateHandler();
            }
            std::abort();
        }
    } // namespace

    bool CrashScene::install() noexcept
    {
        // 备用信号栈与信号处置归 Platform；这里只登记现场文本的生产者与 terminate 链
        if (!Platform::CrashHandler::install(&reportMinimalSignalScene, &reportFullSignalScene))
        {
            return false;
        }
        g_previousTerminateHandler = std::set_terminate(&handleTerminate);
        return true;
    }

    bool CrashScene::isInstalled() noexcept
    {
        return Platform::CrashHandler::isInstalled();
    }
} // namespace AsynGyanis::Core
