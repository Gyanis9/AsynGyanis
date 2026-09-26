// Base/Exception 全模块单元测试：各异常类的构造、属性、继承关系与压力场景。

#include "Base/Exception/ConfigException.h"
#include "Base/Exception/ConfigFileException.h"
#include "Base/Exception/ConfigKeyNotFoundException.h"
#include "Base/Exception/ConfigParseException.h"
#include "Base/Exception/ConfigValidationException.h"
#include "Base/Exception/Exception.h"
#include "Base/Exception/ExceptionStackTrace.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/LogicException.h"
#include "Base/Exception/NetworkException.h"
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

        EXPECT_THROW(throw exception, std::runtime_error);
        EXPECT_THROW(throw exception, std::exception);
        EXPECT_THROW(throw exception, Exception);
    }

    TEST(Exception, MessageCarriesLocationOfThrow)
    {
        const auto      sourceLocation = std::source_location::current();
        const Exception exception("whereami", sourceLocation);

        const std::string message(exception.what());
        EXPECT_TRUE(contains(message, "[异常]"));
        EXPECT_TRUE(contains(message, "whereami"));
        EXPECT_TRUE(contains(message, sourceLocation.file_name()));
        EXPECT_TRUE(contains(message, std::to_string(sourceLocation.line())));
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
        // 按**帧身份**判，而不是「栈里有个测试体帧就算对」：任何深于两帧的栈里都有 TestBody，
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

        EXPECT_THROW(throw exception, Exception);
        EXPECT_THROW(throw exception, std::runtime_error);
        EXPECT_THROW(throw exception, std::exception);
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
    // ConfigFileException
    // ============================================================================

    TEST(ConfigFileException, InheritsFromConfigException)
    {
        const ConfigFileException exception("/path/to/file", "permission denied");

        EXPECT_THROW(throw exception, ConfigException);
        EXPECT_THROW(throw exception, std::runtime_error);
    }

    TEST(ConfigFileException, ExposesPathAndMentionsBothFields)
    {
        const ConfigFileException exception("/etc/config.yaml", "permission denied");
        const std::string         message(exception.what());

        EXPECT_EQ(exception.filePath(), "/etc/config.yaml");
        EXPECT_TRUE(contains(message, "/etc/config.yaml"));
        EXPECT_TRUE(contains(message, "permission denied"));
    }

    TEST(ConfigFileException, AcceptsEmptyPathAndEmptyReason)
    {
        const ConfigFileException emptyPath("", "reason");
        const ConfigFileException emptyReason("file.yaml", "");

        EXPECT_TRUE(emptyPath.filePath().empty());
        EXPECT_EQ(emptyReason.filePath(), "file.yaml");
    }

    TEST(ConfigFileException, KeepsNonAsciiPathIntact)
    {
        const ConfigFileException exception("/配置/文件.yaml", "错误");

        EXPECT_EQ(exception.filePath(), "/配置/文件.yaml");
    }

    // ============================================================================
    // ConfigParseException
    // ============================================================================

    TEST(ConfigParseException, InheritsFromConfigException)
    {
        const ConfigParseException exception("config.yaml", "invalid syntax");

        EXPECT_THROW(throw exception, ConfigException);
        EXPECT_THROW(throw exception, std::runtime_error);
    }

    TEST(ConfigParseException, MessageDescribesParseFailure)
    {
        const ConfigParseException exception("config.yaml", "unexpected token at line 42");
        const std::string          message(exception.what());

        EXPECT_EQ(exception.filePath(), "config.yaml");
        EXPECT_TRUE(contains(message, "config.yaml"));
        EXPECT_TRUE(contains(message, "unexpected token"));
        EXPECT_TRUE(contains(message, "解析错误"));
    }

    TEST(ConfigParseException, HandlesLongReasonAndEmptyFields)
    {
        const std::string          longReason(5000, 'e');
        const ConfigParseException longException("f.yaml", longReason);
        const ConfigParseException emptyException("", "");

        EXPECT_EQ(longException.filePath(), "f.yaml");
        EXPECT_TRUE(emptyException.filePath().empty());
    }

    // ============================================================================
    // ConfigKeyNotFoundException
    // ============================================================================

    TEST(ConfigKeyNotFoundException, InheritsFromConfigException)
    {
        const ConfigKeyNotFoundException exception("server.port");

        EXPECT_THROW(throw exception, ConfigException);
        EXPECT_THROW(throw exception, std::runtime_error);
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

        EXPECT_THROW(throw exception, ConfigException);
        EXPECT_THROW(throw exception, std::runtime_error);
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
    // SystemException 与 NetworkException
    // ============================================================================

    TEST(SystemException, InheritsFromExceptionBase)
    {
        const SystemException exception("system call", std::error_code(2, std::system_category()));

        EXPECT_THROW(throw exception, Exception);
        EXPECT_THROW(throw exception, std::runtime_error);
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

    TEST(SystemException, ReadsErrnoWhenErrorCodeIsOmitted)
    {
        errno = 17;
        const SystemException exception("open handle");

        EXPECT_EQ(exception.nativeError(), 17);
        errno = 0;
    }

    TEST(NetworkException, InheritsFromSystemException)
    {
        const NetworkException exception("connect failed", std::error_code(111, std::system_category()), "127.0.0.1:8080");

        EXPECT_THROW(throw exception, SystemException);
        EXPECT_THROW(throw exception, Exception);
    }

    TEST(NetworkException, ExposesRemoteAddressInMessage)
    {
        const NetworkException exception("handshake", std::error_code(1, std::system_category()), "10.0.0.1:443");
        const std::string      message(exception.what());

        EXPECT_EQ(exception.remoteAddress(), "10.0.0.1:443");
        EXPECT_TRUE(contains(message, "10.0.0.1:443"));
        EXPECT_EQ(exception.nativeError(), 1);
    }

    TEST(NetworkException, FallsBackToErrnoWithoutExplicitCode)
    {
        errno = 111;
        const NetworkException exception("accept", "192.168.0.1:9000");

        EXPECT_EQ(exception.remoteAddress(), "192.168.0.1:9000");
        EXPECT_EQ(exception.nativeError(), 111);
        errno = 0;
    }

    /**
     * @brief 隐式错误码必须按 errno 语义解释：数值与描述得来自同一个错误码空间
     * @details Windows 上 `std::system_category()` 把数值当成 **Win32 码**查表，而 kernel32/winsock
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

    /**
     * @brief 两个 NetworkException 重载都得把对端地址写进消息
     * @details 旧写法只有「显式传码」那条拼 "(remote: X)"，同一次失败因调用方手上有没有错误码
     *          而给出两种文本，而带不带对端的判断恰恰是这个类存在的理由。
     */
    TEST(NetworkException, IncludesRemoteAddressInBothOverloads)
    {
        errno = ECONNABORTED;
        const NetworkException withoutCode("accept", "192.168.1.2:7001");
        const std::string      message(withoutCode.what());
        errno = 0;

        EXPECT_TRUE(contains(message, "192.168.1.2:7001")) << "不显式传码的那条重载把对端丢了：" << message;
        EXPECT_EQ(withoutCode.remoteAddress(), "192.168.1.2:7001");
    }

    // ============================================================================
    // 层次结构与拷贝语义
    // ============================================================================

    TEST(ExceptionHierarchy, EveryConfigExceptionIsCatchableAsStdException)
    {
        // unique_ptr 不可拷贝，故保存工厂而不是实例列表
        const std::vector<std::function<std::unique_ptr<std::exception>()>> factories = {
                [] { return std::unique_ptr<std::exception>(std::make_unique<ConfigException>("test")); },
                [] { return std::unique_ptr<std::exception>(std::make_unique<ConfigFileException>("f", "r")); },
                [] { return std::unique_ptr<std::exception>(std::make_unique<ConfigParseException>("f", "r")); },
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
        const ConfigFileException  original("/path/file.yaml", "test reason");
        const ConfigFileException &copy(original);

        EXPECT_EQ(copy.filePath(), original.filePath());
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
        EXPECT_THROW(throw exception, std::logic_error);
        EXPECT_THROW(throw exception, LogicException);
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
        EXPECT_THROW(throw exception, std::invalid_argument);
        EXPECT_THROW(throw exception, std::logic_error);
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
