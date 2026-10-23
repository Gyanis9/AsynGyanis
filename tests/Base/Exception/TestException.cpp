// Base/Exception 全模块单元测试：各异常类的构造、属性、继承关系与压力场景。

#include "Base/Exception/ConfigException.h"
#include "Base/Exception/ConfigKeyNotFoundException.h"
#include "Base/Exception/ConfigValidationException.h"
#include "Base/Exception/Exception.h"
#include "Base/Exception/ExceptionMessage.h"
#include "Base/Exception/ExceptionStackTrace.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/LogicException.h"
#include "Base/Exception/StackTrace.h"
#include "Base/Exception/SystemException.h"

#include "CommonTestSupport.h"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 断言文本中包含给定子串
         * @param haystack 待检查文本
         * @param needle 期望出现的子串
         * @return true 出现
         */
        bool contains(const std::string &haystack, const std::string &needle)
        {
            return haystack.find(needle) != std::string::npos;
        }

        /**
         * @brief 断言异常可按指定基类捕获，且捕获到的正文与对象自身逐字一致
         * @tparam CatchType 捕获时写的类型
         * @tparam ExceptionType 异常的具体类型（按它抛出，避免按基类抛出时切片）
         * @param exception 被抛出的异常对象
         * @details 只判「能按基类捕获」看不出捕获后正文有没有被包一层或截断；分类前缀与抛出点
         *          正是下游日志依赖的部分，两条一起判
         */
        template<typename CatchType, typename ExceptionType>
        void expectCatchableWithSameText(const ExceptionType &exception)
        {
            try
            {
                throw exception;
            } catch (const CatchType &caught)
            {
                EXPECT_STREQ(caught.what(), exception.what()) << "按基类捕获后正文变了";
                return;
            } catch (...)
            {
                ADD_FAILURE() << "异常没有落在期望的基类上";
            }
        }

        /**
         * @brief 从「[异常] 消息 [文件:行 in 函数]」的外壳里取出文件段
         * @param message 异常 what() 原文
         * @return std::string 外壳里冒号之前的那一段；外壳不在时返回空串
         * @details 按位置切而不是拿整个字符串去比对：函数名各编译器写法不同（模板与 lambda 尤甚），
         *          拼一整句会让用例把编译器的格式当成契约
         */
        [[nodiscard]] std::string fileSegmentOf(const std::string &message)
        {
            const std::size_t shellStart = message.rfind(" [");
            if (shellStart == std::string::npos)
            {
                return {};
            }
            const std::size_t fileStart = shellStart + 2U;
            const std::size_t colon     = message.find(':', fileStart);
            return colon == std::string::npos ? std::string{} : message.substr(fileStart, colon - fileStart);
        }

        /**
         * @brief 在独立函数里构造异常：调用栈用例靠它确认「捕获到的是抛出点，不是打印点」
         */
        [[nodiscard]] Exception makeExceptionFromDeepFrame()
        {
            return Exception("stack probe");
        }
    } // namespace

    // ============================================================================
    // Exception 基类
    // ============================================================================

    TEST(Exception, InheritsFromRuntimeError)
    {
        Exception exception("base error");

        expectCatchableWithSameText<std::runtime_error>(exception);
        expectCatchableWithSameText<std::exception>(exception);
        expectCatchableWithSameText<Exception>(exception);
    }

    TEST(Exception, MessageCarriesLocationOfThrow)
    {
        const auto      sourceLocation = std::source_location::current();
        const Exception exception("whereami", sourceLocation);

        const std::string message(exception.what());
        EXPECT_TRUE(contains(message, "[异常]"));
        EXPECT_TRUE(contains(message, "whereami"));
        EXPECT_TRUE(contains(message, "TestException.cpp")) << message;
        EXPECT_TRUE(contains(message, std::to_string(sourceLocation.line())));
    }

    TEST(Exception, MessageCarriesBareFileNameAndNoBuildPath)
    {
        const auto      sourceLocation = std::source_location::current();
        const Exception exception("whereami", sourceLocation);

        // 绝对路径是编译期烘进二进制的：本机的 G:\Codes\... 或容器里的 /... 一旦进了 what()，
        // 抄进日志就跨机器对不上，回给对端就是白送一份构建目录。这里要的是末段，一个字不多
        EXPECT_EQ(fileSegmentOf(exception.what()), "TestException.cpp");
    }

    TEST(ExceptionMessage, LastPathSegmentKeepsOnlyTheLeaf)
    {
        // 两条分隔符都要认：MSVC 展开的是反斜杠路径，GCC 是正斜杠
        EXPECT_EQ(Detail::lastPathSegment(R"(G:\Codes\AsynGyanis\src\Base\Exception\Exception.cpp)"), "Exception.cpp");
        EXPECT_EQ(Detail::lastPathSegment("/home/build/src/Base/LogicException.cpp"), "LogicException.cpp");
        // 没有分隔符与空串是真实形状：某些构建下 file_name() 本来就是裸文件名
        EXPECT_EQ(Detail::lastPathSegment("Socket.cpp"), "Socket.cpp");
        EXPECT_EQ(Detail::lastPathSegment(""), "");
        // 结尾带分隔符时给出空串：那说明整条路径没有末段，硬撑一个「最后一个目录名」会读成假文件名
        EXPECT_EQ(Detail::lastPathSegment("/var/log/"), "");
    }

    TEST(Exception, LocationAccessorMatchesConstructorArgument)
    {
        const auto      sourceLocation = std::source_location::current();
        const Exception exception("stored location", sourceLocation);

        EXPECT_EQ(exception.location().line(), sourceLocation.line());
        EXPECT_STREQ(exception.location().file_name(), sourceLocation.file_name());
    }

    TEST(Exception, DefaultLocationPointsAtTestCallSite)
    {
        const Exception exception("implicit location");

        EXPECT_GT(exception.location().line(), 0U);
        EXPECT_NE(std::string(exception.location().function_name()).find("TestBody"), std::string::npos);
    }

    // ============================================================================
    // 调用栈：构造时捕获抛出点（原始帧），符号解析推迟到输出时
    // ============================================================================

    TEST(Exception, CapturesThrowSiteStackTraceAtConstruction)
    {
        const Exception exception = makeExceptionFromDeepFrame();

        if (exception.stackTrace().empty())
        {
            GTEST_SKIP() << "本构建未启用 std::stacktrace（ASYN_HAS_STACKTRACE 未定义），栈按空实现退化";
        }
        EXPECT_GE(exception.stackTrace().size(), 1U) << "构造时应至少捕获到抛出点一帧";
    }

    TEST(Exception, StackTraceTextResolvesTheThrowingFunction)
    {
        const Exception   exception = makeExceptionFromDeepFrame();
        const std::string text      = formatStackTrace(exception.stackTrace());

        if (!TestSupport::hasResolvedStackTraceFrames(text))
        {
            GTEST_SKIP() << "本构建没有调试信息（既无 PDB 也无 -g），栈帧只剩模块加偏移，符号解析断言不适用";
        }
        // 按帧身份判，而不是「栈里有个测试体帧就算对」：任何深于两帧的栈里都有 TestBody，
        // 旧断言在「少跳一格」（框架自己的构造帧顶在最前）与「多跳一格」（抛出点被跳掉）两种
        // 错法下都不会红。这里两条分别钉住：抛出点所在文件必须在栈里，框架构造帧必须不在
        EXPECT_NE(text.find("TestException.cpp"), std::string::npos) << "解析结果里没有抛出点所在文件，说明采到的不是抛出点的栈：\n" << text;
        EXPECT_EQ(text.find("Exception::Exception"), std::string::npos) << "栈里露出了框架自己的构造帧，说明 captureStackTrace 少跳了一格：\n" << text;
    }

    TEST(LogicException, CapturesThrowSiteStackTrace)
    {
        const LogicException exception("用法错误");

        if (exception.stackTrace().empty())
        {
            GTEST_SKIP() << "本构建未启用 std::stacktrace（降级为空实现）";
        }
        EXPECT_GE(exception.stackTrace().size(), 1U) << "用法错误这条链同样要携带抛出点栈";
    }

    // ============================================================================
    // tryStackTrace：只 catch 到 std::exception 时取回抛出点栈（日志宏的异常出口靠它）
    // ============================================================================

    TEST(ExceptionStackTraceAccess, ResolvesEveryFrameworkChainToItsOwnStackTrace)
    {
        const Exception                runtimeFailure("运行期故障");
        const LogicException           usageFailure("用法错误");
        const InvalidArgumentException invalidValue("取值非法");
        // 派生类也要认得：ConfigValidationException 只是借基类带上栈，识别依据是运行期类型
        const ConfigValidationException validationFailure("server.port", "取值超出范围");

        EXPECT_NE(tryStackTrace(runtimeFailure), nullptr) << "这条链漏识别时，日志里只剩消息没有栈";
        EXPECT_NE(tryStackTrace(usageFailure), nullptr) << "logic_error 分支漏识别";
        EXPECT_NE(tryStackTrace(invalidValue), nullptr) << "InvalidArgumentException 与 LogicException 是兄弟而非父子，只能各自识别一次";
        EXPECT_NE(tryStackTrace(validationFailure), nullptr) << "按基类接口传进来的派生类同样要取到栈";

        // 交回的必须是异常自己持有的那一份而不是副本，否则解析出的帧与调用方拿到的不是同一次采样
        EXPECT_EQ(tryStackTrace(runtimeFailure), &runtimeFailure.stackTrace());
        EXPECT_EQ(tryStackTrace(invalidValue), &invalidValue.stackTrace());
    }

    TEST(ExceptionStackTraceAccess, ReturnsNullForForeignExceptions)
    {
        const std::runtime_error foreignFailure("标准库异常，不带框架的栈");
        const std::logic_error   foreignUsage("用法错误的标准库分支");

        // Exception 派生自 std::runtime_error，反过来不成立：外来异常不能误报成「有栈可解析」
        EXPECT_EQ(tryStackTrace(foreignFailure), nullptr);
        EXPECT_EQ(tryStackTrace(foreignUsage), nullptr);
    }

    TEST(ExceptionStackTraceAccess, InvalidArgumentChainCarriesTheThrowSiteFrames)
    {
        const InvalidArgumentException invalidValue("取值非法");

        const CapturedStackTrace *captured = tryStackTrace(invalidValue);
        ASSERT_NE(captured, nullptr) << "取不到栈就无从判断这条链有没有在构造时采样";
        if (captured->empty())
        {
            GTEST_SKIP() << "本构建未启用 std::stacktrace（降级为空实现）";
        }
        EXPECT_GE(captured->size(), 1U) << "第三条链也得在构造那一刻采到抛出点帧";
    }

    // ============================================================================
    // ConfigException
    // ============================================================================

    TEST(ConfigException, InheritsFromExceptionBase)
    {
        const ConfigException exception("test error");

        expectCatchableWithSameText<Exception>(exception);
        expectCatchableWithSameText<std::runtime_error>(exception);
        expectCatchableWithSameText<std::exception>(exception);
    }

    TEST(ConfigException, MessageIsPrefixedWithCategory)
    {
        const ConfigException exception("test message");
        const std::string     message(exception.what());

        EXPECT_TRUE(contains(message, "配置错误"));
        EXPECT_TRUE(contains(message, "test message"));
    }

    TEST(ConfigException, AcceptsEmptyMessage)
    {
        const ConfigException exception("");

        EXPECT_FALSE(std::string(exception.what()).empty());
    }

    TEST(ConfigException, HandlesVeryLongMessage)
    {
        const std::string     longMessage(10000, 'X');
        const ConfigException exception(longMessage);

        EXPECT_TRUE(contains(std::string(exception.what()), longMessage));
    }

    // ============================================================================
    // ConfigKeyNotFoundException
    // ============================================================================

    TEST(ConfigKeyNotFoundException, InheritsFromConfigException)
    {
        const ConfigKeyNotFoundException exception("server.port");

        expectCatchableWithSameText<ConfigException>(exception);
        expectCatchableWithSameText<std::runtime_error>(exception);
    }

    TEST(ConfigKeyNotFoundException, ExposesMissingKey)
    {
        const ConfigKeyNotFoundException exception("database.connection.url");
        const std::string                message(exception.what());

        EXPECT_EQ(exception.key(), "database.connection.url");
        EXPECT_TRUE(contains(message, "database.connection.url"));
        EXPECT_TRUE(contains(message, "不存在"));
    }

    TEST(ConfigKeyNotFoundException, AcceptsEmptyKey)
    {
        const ConfigKeyNotFoundException exception("");

        EXPECT_TRUE(exception.key().empty());
    }

    TEST(ConfigKeyNotFoundException, HandlesVariousKeyShapes)
    {
        const std::vector<std::string> candidateKeys = {
                "a", "a.b.c.d.e.f.g.h.i.j", "very_long_key_name_that_exceeds_typical_lengths_for_configuration_keys", "key.with.numbers.123", "key-with-special_chars@test",
        };

        for (const std::string &key: candidateKeys)
        {
            const ConfigKeyNotFoundException exception(key);
            EXPECT_EQ(exception.key(), key);
        }
    }

    // ============================================================================
    // ConfigValidationException
    // ============================================================================

    TEST(ConfigValidationException, InheritsFromConfigException)
    {
        const ConfigValidationException exception("server.port", "must be between 1 and 65535");

        expectCatchableWithSameText<ConfigException>(exception);
        expectCatchableWithSameText<std::runtime_error>(exception);
    }

    TEST(ConfigValidationException, ExposesKeyAndReason)
    {
        const ConfigValidationException exception("server.port", "must be between 1 and 65535");
        const std::string               message(exception.what());

        EXPECT_EQ(exception.key(), "server.port");
        EXPECT_TRUE(contains(message, "must be between 1 and 65535"));
        EXPECT_TRUE(contains(message, "校验失败"));
    }

    TEST(ConfigValidationException, AcceptsEmptyReason)
    {
        const ConfigValidationException exception("key", "");

        EXPECT_EQ(exception.key(), "key");
    }

    // ============================================================================
    // SystemException
    // ============================================================================

    TEST(SystemException, InheritsFromExceptionBase)
    {
        const SystemException exception("system call", std::error_code(2, std::system_category()));

        expectCatchableWithSameText<Exception>(exception);
        expectCatchableWithSameText<std::runtime_error>(exception);
    }

    TEST(SystemException, ExplicitErrorCodeIsExposedAndFormatted)
    {
        const std::error_code errorCode(13, std::system_category());
        const SystemException exception("write config", errorCode);
        const std::string     message(exception.what());

        EXPECT_EQ(exception.errorCode(), errorCode);
        EXPECT_EQ(exception.nativeError(), 13);
        EXPECT_TRUE(contains(message, "write config"));
        EXPECT_TRUE(contains(message, "[13]"));
    }

    /**
     * @brief 统一异常文本：固定前缀、末段文件名与调用行号都在，构建目录不外泄
     */
    TEST(ExceptionMessage, FormatExceptionMessageKeepsLeafNameAndHidesBuildPath)
    {
        const std::source_location location = std::source_location::current();
        const std::string          text     = Detail::formatExceptionMessage("配置缺失", location);

        EXPECT_NE(text.find("[异常] 配置缺失"), std::string::npos) << text;
        // 取方括号里、冒号前的那一段：它必须恰好是文件名。只看「不含 G:\Codes」在 Linux 上是恒真，
        // 突变（换回完整路径）在那一侧就抓不住
        const std::size_t fileBegin = text.rfind('[');
        const std::size_t fileEnd   = text.find(':', fileBegin);
        ASSERT_NE(fileBegin, std::string::npos) << text;
        ASSERT_NE(fileEnd, std::string::npos) << text;
        EXPECT_EQ(text.substr(fileBegin + 1, fileEnd - fileBegin - 1), "TestException.cpp") << "文件必须缩成末段：" << text;
        EXPECT_EQ(text.find("G:\\Codes"), std::string::npos) << "构建目录不该出现在 what() 里：" << text;
        EXPECT_NE(text.find(" in "), std::string::npos) << text;
    }

    TEST(SystemException, ReadsErrnoWhenErrorCodeIsOmitted)
    {
        errno = 17;
        const SystemException exception("open handle");

        EXPECT_EQ(exception.nativeError(), 17);
        errno = 0;
    }

    /**
     * @brief 公开的 errno 取码入口也必须交回 errno 语义的类别
     */
    TEST(SystemException, LastErrnoErrorCodeUsesTheErrnoCategory)
    {
        errno                           = ENOENT;
        const std::error_code errorCode = SystemException::lastErrnoErrorCode();
        errno                           = 0;

        EXPECT_EQ(errorCode.value(), ENOENT);
        EXPECT_EQ(errorCode.category(), std::generic_category()) << "类别写成 system_category() 会让 Windows 按 Win32 码解释";
    }

    /**
     * @brief 隐式错误码必须按 errno 语义解释：数值与描述得来自同一个错误码空间
     * @details Windows 上 `std::system_category()` 把数值当成 Win32 码查表，而 kernel32/winsock
     *          的失败根本不写 errno，于是旧写法（errno 的值 + system_category）会给出与本次失败
     *          无关的描述（本仓实测把发送失败报成「[112] There is not enough space on the disk」）。
     *          Linux 上两个类别的文本相同，因此这条只有 Windows 侧能证伪——缺陷本身也只在那一侧。
     */
    TEST(SystemException, ErrnoValueIsDescribedWithTheErrnoCategory)
    {
        errno = EACCES;
        const SystemException exception("write file");
        const std::string     message(exception.what());
        errno = 0;

        const std::error_code errnoSemantics(EACCES, std::generic_category());
        EXPECT_EQ(exception.errorCode().category(), errnoSemantics.category()) << "类别与取值不配对，报出来的描述就不是这次失败";
        EXPECT_TRUE(contains(message, errnoSemantics.message())) << "消息里的描述与 errno 语义不符（多半是按 Win32 码查的表）：" << message;
    }

    // ============================================================================
    // 层次结构与拷贝语义
    // ============================================================================

    TEST(ExceptionHierarchy, EveryConfigExceptionIsCatchableAsStdException)
    {
        // unique_ptr 不可拷贝，故保存工厂而不是实例列表
        const std::vector<std::function<std::unique_ptr<std::exception>()>> factories = {
                [] { return std::unique_ptr<std::exception>(std::make_unique<ConfigException>("test")); },
                [] { return std::unique_ptr<std::exception>(std::make_unique<ConfigKeyNotFoundException>("k")); },
                [] { return std::unique_ptr<std::exception>(std::make_unique<ConfigValidationException>("k", "r")); },
        };

        for (const auto &factory: factories)
        {
            const std::unique_ptr<std::exception> exception = factory();

            ASSERT_NE(exception, nullptr);
            EXPECT_FALSE(std::string(exception->what()).empty());
        }
    }

    TEST(ExceptionHierarchy, CopyPreservesMessageAndFields)
    {
        // 这条例用例原先挂在已删除的 ConfigFileException 上；换到仍有两个字段（键与原因）的
        // ConfigValidationException，「按引用拷贝后消息与字段都保真」这条覆盖不因此消失
        const ConfigValidationException  original("server.port", "test reason");
        const ConfigValidationException &copy(original);

        EXPECT_EQ(copy.key(), original.key());
        EXPECT_STREQ(copy.what(), original.what());
    }

    TEST(ExceptionHierarchy, SlicedCatchStillReportsMessage)
    {
        const ConfigKeyNotFoundException original("slice.key");
        const Exception                 &sliced(original);

        EXPECT_TRUE(contains(std::string(sliced.what()), "slice.key"));
    }

    // ============================================================================
    // 压力场景
    // ============================================================================

    TEST(ExceptionStress, RapidThrowCatchKeepsMessagesDistinct)
    {
        constexpr int kiterationCount = 10000;

        for (int iteration = 0; iteration < kiterationCount; ++iteration)
        {
            try
            {
                throw ConfigKeyNotFoundException("key_" + std::to_string(iteration));
            } catch (const ConfigException &exception)
            {
                ASSERT_FALSE(std::string(exception.what()).empty());
            }
        }
    }

    TEST(ExceptionStress, ThrowingDerivedIsCaughtByBaseReference)
    {
        int caughtCount = 0;

        for (int iteration = 0; iteration < 500; ++iteration)
        {
            try
            {
                throw ConfigValidationException("k", "类型不匹配：期望 int，实际 string");
            } catch (const Exception &exception)
            {
                EXPECT_TRUE(contains(std::string(exception.what()), "类型不匹配"));
                ++caughtCount;
            }
        }

        EXPECT_EQ(caughtCount, 500);
    }

    // ============================================================================
    // LogicException / InvalidArgumentException：std::logic_error 这条分支
    // ============================================================================

    TEST(LogicException, SitsOnTheLogicErrorBranchNotTheRuntimeErrorOne)
    {
        // 继承关系本身就是设计：编程/用法错误留在 std::logic_error 分支上，
        // 与派生自 std::runtime_error 的 Exception 平行，互不包含
        static_assert(std::is_base_of_v<std::logic_error, LogicException>);
        static_assert(!std::is_base_of_v<Exception, LogicException>);
        static_assert(!std::is_base_of_v<std::runtime_error, LogicException>);

        const LogicException exception("离线模式下不允许执行该操作");
        expectCatchableWithSameText<std::logic_error>(exception);
        expectCatchableWithSameText<LogicException>(exception);
    }

    TEST(LogicException, MessageUsesSharedFormatAndKeepsThrowLocation)
    {
        const std::source_location throwSite = std::source_location::current();
        const LogicException       exception("表结构不合法", throwSite);

        const std::string message = exception.what();
        EXPECT_TRUE(contains(message, "[异常]")) << message;
        EXPECT_TRUE(contains(message, "表结构不合法")) << message;
        EXPECT_EQ(exception.location().line(), throwSite.line());
        EXPECT_STREQ(exception.location().file_name(), throwSite.file_name());
    }

    TEST(InvalidArgumentException, IsCatchableAsBothStandardTypes)
    {
        // 派生自 std::invalid_argument，而后者又派生自 std::logic_error：
        // 按标准分类的上游处理器两种写法都能命中
        static_assert(std::is_base_of_v<std::invalid_argument, InvalidArgumentException>);
        static_assert(std::is_base_of_v<std::logic_error, InvalidArgumentException>);
        static_assert(!std::is_base_of_v<Exception, InvalidArgumentException>);

        const InvalidArgumentException exception("取值个数与列数不一致");
        expectCatchableWithSameText<std::invalid_argument>(exception);
        expectCatchableWithSameText<std::logic_error>(exception);
    }

    TEST(LogicException, DoesNotCatchItsSiblingInvalidArgumentException)
    {
        // 两个类型各自继承标准库的两条分支，无法合成一条（会形成菱形基类），因此是兄弟而非父子。
        // 要一次网住「所有用法错误」，捕获它们的共同基类 std::logic_error 才是正确写法
        try
        {
            throw InvalidArgumentException("取值个数与列数不一致");
        } catch (const LogicException &)
        {
            FAIL() << "InvalidArgumentException 不应被 LogicException 捕获：二者是兄弟类型";
        } catch (const std::logic_error &exception)
        {
            EXPECT_TRUE(contains(exception.what(), "取值个数与列数不一致"));
        }
    }
} // namespace AsynGyanis::Base
