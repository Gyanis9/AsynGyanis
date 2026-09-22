// Console 单元测试：UTF-8 输出代码页设置与 ANSI 转义能力判定
#include "Platform/IO/Console.h"

#include "Platform/System/ProcessInfo.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

#if !ASYN_PLATFORM_WIN32
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace AsynGyanis::Platform
{
    namespace
    {
#if !ASYN_PLATFORM_WIN32
        /**
         * @brief 把标准输出临时接到一条伪终端的从端上，析构时接回去
         * @details 判据需要「stdout 真的挂在终端上」这一档，而 ctest 一律把它捕获成管道——
         *          不自己造出这一条件，`supportsAnsiEscapeCodes()` 的 true 分支就永远跑不到。
         *          主端全程不读：只需要从端是个 tty，不需要有人消费写进去的字节。
         */
        class StandardOutputOnPseudoTerminal
        {
        public:
            StandardOutputOnPseudoTerminal()
            {
                m_master = ::posix_openpt(O_RDWR | O_NOCTTY);
                if (m_master < 0 || ::grantpt(m_master) != 0 || ::unlockpt(m_master) != 0)
                {
                    return;
                }
                char slavePath[PATH_MAX]{};
                if (::ptsname_r(m_master, slavePath, sizeof(slavePath)) != 0)
                {
                    return;
                }
                m_slave = ::open(slavePath, O_RDWR | O_NOCTTY);
                if (m_slave < 0)
                {
                    return;
                }
                m_savedStandardOutput = ::dup(STDOUT_FILENO);
                if (m_savedStandardOutput < 0 || ::dup2(m_slave, STDOUT_FILENO) < 0)
                {
                    return;
                }
                m_isAttached = true;
            }

            StandardOutputOnPseudoTerminal(const StandardOutputOnPseudoTerminal &) = delete;
            StandardOutputOnPseudoTerminal &operator=(const StandardOutputOnPseudoTerminal &) = delete;

            /**
             * @brief 立刻把标准输出接回原来的目标（析构里也会兜一次）
             * @details 断言要等到接回之后才落下：接管期间的失败消息会写进没人读的伪终端，
             *          用例变红了也看不到原因。
             */
            void restore() noexcept
            {
                if (m_savedStandardOutput < 0)
                {
                    return;
                }
                // 先把这条临时管道上的缓冲冲掉再接回去，否则判定期间落在 stdout 缓冲里的字节会
                // 在 fd 已经换主之后才写出，去向就成了下一条挂在标准输出上的东西
                static_cast<void>(std::fflush(nullptr));
                static_cast<void>(::dup2(m_savedStandardOutput, STDOUT_FILENO));
                static_cast<void>(::close(m_savedStandardOutput));
                m_savedStandardOutput = -1;
            }

            ~StandardOutputOnPseudoTerminal()
            {
                restore();
                if (m_slave >= 0)
                {
                    static_cast<void>(::close(m_slave));
                }
                if (m_master >= 0)
                {
                    static_cast<void>(::close(m_master));
                }
            }

            /// @return true 伪终端已接管标准输出
            [[nodiscard]] bool isAttached() const noexcept
            {
                return m_isAttached;
            }

        private:
            int  m_master{-1};               ///< 伪终端主端，全程不读
            int  m_slave{-1};                ///< 伪终端从端，被 dup2 到标准输出上
            int  m_savedStandardOutput{-1};  ///< 接管之前标准输出的副本
            bool m_isAttached{false};        ///< 是否已完成接管
        };
#endif
    } // namespace

    TEST(Console, EnsureUtf8OutputIsRepeatableAndSafe)
    {
        EXPECT_NO_THROW(Console::ensureUtf8Output());
        EXPECT_NO_THROW(Console::ensureUtf8Output());
    }

    TEST(Console, SupportsAnsiEscapeCodesIsStableAcrossCalls)
    {
        const bool firstResult  = Console::supportsAnsiEscapeCodes();
        const bool secondResult = Console::supportsAnsiEscapeCodes();

        // 同一进程内标准输出的目标不会变化，能力判定必须给出一致结论
        EXPECT_EQ(firstResult, secondResult);
    }

    TEST(Console, SupportsAnsiEscapeCodesDoesNotThrowWhenOutputIsCaptured)
    {
        // 测试运行时标准输出通常被 ctest 捕获为管道：必须安全返回 false 而不是崩溃
        EXPECT_NO_THROW((void)Console::supportsAnsiEscapeCodes());
    }

#if !ASYN_PLATFORM_WIN32
    /**
     * @brief 钉住：接在终端上时按 TERM 分得出「能用彩色」与「dumb 终端」
     * @details 两条出口此前都没有直测：管道下只能看到 false，看不出「因为不是终端所以 false」与
     *          「是终端但 TERM=dumb 所以 false」是同一段代码里的两个判定。造出 tty 之后按可用名、
     *          dumb 与缺失三档各测一次，顺带钉住「TERM 每次现读」——运行期改环境变量确实该改判结果
     *          （与本层不缓存外部能力的口径一致）。
     * @note 断言一律等标准输出接回去之后再落下：失败消息若在接管期间产生，就写进没人读的伪终端里，
     *       用例变红了也看不到原因。
     */
    TEST(Console, SupportsAnsiEscapeCodesFollowsTerminalAndTermVariable)
    {
        StandardOutputOnPseudoTerminal terminal;
        if (!terminal.isAttached())
        {
            GTEST_SKIP() << "本机建不出伪终端（/dev/pts 不可用），这一档条件无从构造";
        }

        const std::optional<std::string> previousTerminalName = ProcessInfo::environmentVariable("TERM");
        // 三档判定都先把结果取进变量，等标准输出接回去之后再断言（原因见本用例的 @note）
        const bool usableTerminalSet  = ::setenv("TERM", "xterm-256color", 1) == 0;
        const bool onUsableTerminal   = Console::supportsAnsiEscapeCodes();
        const bool dumbTerminalSet    = ::setenv("TERM", "dumb", 1) == 0;
        const bool onDumbTerminal     = Console::supportsAnsiEscapeCodes();
        const bool termVariableRemoved = ::unsetenv("TERM") == 0;
        const bool onMissingTerm      = Console::supportsAnsiEscapeCodes();
        // 把环境恢复原样：同一进程里后续跑到的判定不该看到被本用例改掉的 TERM
        if (previousTerminalName.has_value())
        {
            static_cast<void>(::setenv("TERM", previousTerminalName->c_str(), 1));
        }

        terminal.restore();
        ASSERT_TRUE(usableTerminalSet && dumbTerminalSet && termVariableRemoved)
                << "改环境变量就失败了，下面三档判定的条件没造出来";
        EXPECT_TRUE(onUsableTerminal) << "接在终端上且 TERM 是可用终端名，还报「不支持」就等于永远不出彩色";
        EXPECT_FALSE(onDumbTerminal) << "TERM=dumb 必须退回纯文本，否则转义序列会以原字符打进只认字面的终端";
        EXPECT_FALSE(onMissingTerm) << "TERM 缺失时无从断定终端能力，按不支持处理才不会污染输出";
    }
#endif
} // namespace AsynGyanis::Platform

