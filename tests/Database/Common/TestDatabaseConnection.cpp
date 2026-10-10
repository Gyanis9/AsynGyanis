// DatabaseConnection 抽象基类的直测：executeChecked 把「空指针 + lastError()」换成带着成因的
// 返回值。钉住四件事——成功时原样交出结果集、失败时文本与驱动原生码成对带回、驱动没留下任何
// 文本时落到一句兜底说明、基类那条「本驱动不支持参数化查询」的提示照样抄得回来。
// 后三条是包装自身的形状，只能靠一个把错误摆好的驱动替身打出来；成功一侧走真实 SQLite 驱动。

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseType.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Sqlite/SqliteConnection.h"

#include "DatabaseTestSupport.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Database
{
    namespace
    {
        using TestSupport::containsLocalizedText;

        /**
         * @brief 一个把「错误」按用例摆好的驱动替身
         * @details 只用来打包装自身的三条形状：lastError() 与 lastNativeErrorCode() 读的是同一份
         *          ErrorRecord，替身按真驱动的写法填它即可。**刻意不重写带参数的 execute()，
         *          也不重写 executeStreaming()**，那样基类那两条默认实现（「暂不支持参数化查询」与
         *          「暂不支持流式结果集」）才走得到。
         */
        class StubConnection final : public DatabaseConnection
        {
        public:
            /**
             * @brief 建立连接：替身没有底层句柄，恒成功
             * @return true 总是
             */
            bool connect() override
            {
                return true;
            }

            /**
             * @brief 断开连接：空实现
             */
            void disconnect() override
            {
            }

            /**
             * @brief 连接状态：替身恒称已连接（本文件只测执行结果的交接口径，不测建连）
             * @return true 总是
             */
            [[nodiscard]] bool isConnected() const override
            {
                return true;
            }

            /**
             * @brief 本替身冒充的驱动类型
             * @return DatabaseType 恒为 Sqlite，只为让提示里的驱动名有确定取值
             */
            DatabaseType databaseType() const override
            {
                return DatabaseType::Sqlite;
            }

            /**
             * @brief 回空指针，并把此前摆进 m_lastError 的原因留着
             * @param command 命令文本（替身不解析）
             * @return std::unique_ptr<DatabaseResult> 恒为空指针：失败形状是本替身的全部职责
             */
            std::unique_ptr<DatabaseResult> execute(const std::string_view command) override
            {
                static_cast<void>(command);
                ++m_executeCallCount;
                return nullptr;
            }

            /// 摆一条「带文本带码」的失败，形状与真驱动一致
            void failWith(const std::string &text, const std::int64_t nativeCode)
            {
                m_lastError.assignNative(text, nativeCode);
            }

            /// 摆一条「只回空指针、什么都没写」的失败：驱动侧已知的退化情形
            void failWithoutReason()
            {
                m_lastError.clear();
            }

            /// execute() 被叫到的次数：证明包装没有把命令重发一遍
            [[nodiscard]] std::size_t executeCallCount() const noexcept
            {
                return m_executeCallCount;
            }

        private:
            std::size_t m_executeCallCount{0U}; ///< execute() 被调用的次数
        };
    } // namespace

    // ============================================================================
    // executeChecked：失败一侧带回成因
    // ============================================================================

    TEST(DatabaseConnectionTest, ExecuteCheckedPairsTextWithTheCodeFromTheSameRecord)
    {
        StubConnection connection;
        connection.failWith("执行 SQL 命令失败：死锁（错误码 1213）", 1213);

        const auto outcome = connection.executeChecked("UPDATE t SET x = 1");
        ASSERT_FALSE(outcome.has_value());
        // 文本与码出自同一条记录：判「该不该重试」要的是码，而码必须对得上这句文本
        EXPECT_NE(outcome.error().message.find("1213"), std::string::npos) << outcome.error().message;
        EXPECT_EQ(outcome.error().nativeCode, 1213);
        EXPECT_TRUE(containsLocalizedText(outcome.error().message)) << outcome.error().message;
        EXPECT_EQ(connection.executeCallCount(), 1U) << "包装把命令发了两遍";
    }

    TEST(DatabaseConnectionTest, ExecuteCheckedFallsBackWhenTheDriverLeftNoText)
    {
        StubConnection connection;
        connection.failWithoutReason();

        const auto outcome = connection.executeChecked("SELECT 1");
        ASSERT_FALSE(outcome.has_value());
        // 空原因的失败比原因本身更难查：兜底一句，且码同为「未知」而不是上一条的码
        EXPECT_FALSE(outcome.error().message.empty()) << "驱动没留文本时交回了空原因";
        EXPECT_TRUE(containsLocalizedText(outcome.error().message));
        EXPECT_EQ(outcome.error().nativeCode, DatabaseConnection::ErrorRecord::kUnknownNativeCode);
    }

    TEST(DatabaseConnectionTest, ExecuteCheckedReportsTheBaseClassParameterSupportRefusal)
    {
        StubConnection                     connection;
        const std::array<DatabaseValue, 1> parameters{DatabaseValue{std::int64_t{1}}};

        // 本替身没有带参数的实现，走到的是基类那条默认实现写下的中文提示
        const auto outcome = connection.executeChecked("SELECT ?", std::span<const DatabaseValue>{parameters});
        ASSERT_FALSE(outcome.has_value());
        EXPECT_TRUE(containsLocalizedText(outcome.error().message)) << outcome.error().message;
        EXPECT_NE(outcome.error().message.find("Sqlite"), std::string::npos) << "提示里没说是哪个驱动不支持";
    }

    /**
     * @brief 钉住基类那条「暂不支持流式结果集」的默认实现：不退化成预读，也不把命令发出去
     * @details 退化成 execute() 是这条接口最坏的失败形态——调用方以为自己按行取数，实际整份结果
     *          仍被搬进内存，而内存量级正是它选择流式读的唯一理由。因此默认实现只写原因、交空指针。
     * @note 证伪：把默认实现改成 `return execute(command);`，本条红在「交出了结果集」与「命令被发了出去」两句上
     */
    TEST(DatabaseConnectionTest, StreamingExecutionReportsTheBaseClassRefusalForDriversThatDoNotImplementIt)
    {
        StubConnection connection;

        const std::unique_ptr<DatabaseResult> stream = connection.executeStreaming("SELECT 1");
        EXPECT_EQ(stream, nullptr) << "驱动没实现流式读却交出了结果集：默认实现退化成了预读";
        EXPECT_EQ(connection.executeCallCount(), 0U) << "默认实现把命令交给 execute() 发出去了，那正是「静默退回整份预读」";
        EXPECT_TRUE(containsLocalizedText(connection.lastError())) << connection.lastError();
        EXPECT_NE(connection.lastError().find("Sqlite"), std::string::npos) << "提示里没说是哪个驱动不支持";
    }

    // ============================================================================
    // executeChecked：成功一侧原样交出结果（真实驱动）
    // ============================================================================

    TEST(DatabaseConnectionTest, ExecuteCheckedPassesTheResultSetThroughOnSuccess)
    {
        SqliteConnection connection(ConnectionConfig::sqliteDefault());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        const auto created = connection.executeChecked("CREATE TABLE probe (n INTEGER)");
        ASSERT_TRUE(created.has_value()) << created.error().message;
        ASSERT_NE(*created, nullptr);

        const auto selected = connection.executeChecked("SELECT 41 + 1");
        ASSERT_TRUE(selected.has_value()) << selected.error().message;
        ASSERT_NE(*selected, nullptr);
        ASSERT_TRUE((*selected)->next());
        EXPECT_EQ(TestSupport::asInteger((*selected)->getValue(0U)), 42);
    }

    TEST(DatabaseConnectionTest, ExecuteCheckedCarriesTheRealDriverCodeOnFailure)
    {
        SqliteConnection connection(ConnectionConfig::sqliteDefault());
        ASSERT_TRUE(connection.connect()) << connection.lastError();

        const auto outcome = connection.executeChecked("SELEKT 1");
        ASSERT_FALSE(outcome.has_value());
        EXPECT_TRUE(containsLocalizedText(outcome.error().message)) << outcome.error().message;
        // 真驱动的码要成对带回来；文本里本来就拼着同一个码（composeNativeErrorText 的形状）
        EXPECT_EQ(outcome.error().nativeCode, connection.lastNativeErrorCode());
        EXPECT_NE(outcome.error().message.find("错误码"), std::string::npos) << outcome.error().message;
    }
} // namespace AsynGyanis::Database
