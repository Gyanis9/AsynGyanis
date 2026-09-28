// 真机构用例：只走「取目录 + 建号 / 复用账户」这一段，不打扰它签发证书。
// 覆盖的是桩机构测不到的那一半：出站 TLS 要过真信任库、目录字段以线上实际写法为准，尤其是我们的
// JWS 由一个**不是自己写的**裁判验签——桩机构与实现同源，两边错在同一种理解上时桩会跟着一起绿，
// 只有真机构能把这类型一致错误浮出来。
// 门控：三个环境变量缺一不可，缺任何一条即 SKIP。仓库零明文凭据，且一次普通回归不会在机构侧
//       多出真账户：Let's Encrypt 对建号有速率限制（每 3 小时 10 万个），所以要求账户密钥与账户 URL
//       落在一处跨轮次持久的目录里，第二轮之后都走「复用已有账户」那条出口。
// 跑法（示例用 staging 端点：它不计入生产速率限制，签出的证书不被浏览器信任）：
//   ASYN_ACME_TEST_DIRECTORY_URL=https://acme-staging-v02.api.letsencrypt.org/directory
//   ASYN_ACME_TEST_CONTACT_EMAIL=mailto:ops@example.com
//   ASYN_ACME_TEST_ACCOUNT_STATE_DIR=<一个可写的持久目录>
//   ctest -R AcmeLiveAuthorityTest
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Net/Acme/AcmeClient.h"
#include "Net/Acme/AcmeKeyPair.h"
#include "Platform/FileSystem/AtomicFileWriter.h"
#include "Platform/IO/FileContents.h"
#include "Platform/System/ProcessInfo.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 机构目录 URL 的环境变量名。填生产端点之前想清楚：真签发的那一步会被生产端计数
        constexpr const char *kDirectoryUrlVariableName = "ASYN_ACME_TEST_DIRECTORY_URL";

        /// 注册账户用的联系邮箱，按 RFC 8555 §7.3 写成 "mailto:…"
        constexpr const char *kContactEmailVariableName = "ASYN_ACME_TEST_CONTACT_EMAIL";

        /// 账户材料的持久目录；缺它就不建号
        constexpr const char *kAccountStateDirectoryVariableName = "ASYN_ACME_TEST_ACCOUNT_STATE_DIR";

        /// 账户密钥的文件名（PKCS#8 PEM，0600）
        constexpr const char *kAccountKeyFileName = "acme-live-account-key.pem";

        /// 账户 URL 的文件名：两轮都成功之后写下，下一轮就从复用出口起步
        constexpr const char *kAccountUrlFileName = "acme-live-account-url.txt";

        /// 单次出站请求的时限：机构在公网另一侧，比桩那一档放宽
        constexpr std::chrono::milliseconds kRequestTimeout{20000};

        /**
         * @brief 读一个环境变量，空串按「没设」算
         */
        [[nodiscard]] std::optional<std::string> readVariable(const char *variableName)
        {
            std::optional<std::string> value = Platform::ProcessInfo::environmentVariable(variableName);
            if (!value.has_value() || value->empty())
            {
                return std::nullopt;
            }
            return value;
        }
    } // namespace

    /**
     * @brief 真机构的门控用例
     * @details 账户密钥在 SetUp 里备好（生成 RSA-2048 是百毫秒级的整数运算，不占循环线程）。
     *          客户端、结果与循环都是成员：驱动协程按引用捕获它们，而协程帧活到循环停下之后，
     *          放进局部作用域就是让帧去读已析构的对象。
     */
    class AcmeLiveAuthorityTest : public ::testing::Test
    {
    protected:
        /**
         * @brief 一次真机流程的产出
         */
        struct Outcome
        {
            bool        isFirstRoundValid{false};  ///< 第一轮（新建或按持久 URL 复用）是否成功
            bool        isSecondRoundValid{false}; ///< 第二轮（一定带着第一轮的 URL）是否成功
            std::string firstRoundAccountUrl{};    ///< 第一轮结束时的账户 URL
            std::string secondRoundAccountUrl{};   ///< 第二轮结束时的账户 URL
            std::string failureMessage{};          ///< 第一条失败原因，含机构原文
        };

        void SetUp() override
        {
            // 三条门控都在测试函数本体里判（GTEST_SKIP 放进助手不生效），缺任何一条就整组跳过
            const std::optional<std::string> directoryUrl   = readVariable(kDirectoryUrlVariableName);
            const std::optional<std::string> contactEmail   = readVariable(kContactEmailVariableName);
            const std::optional<std::string> stateDirectory = readVariable(kAccountStateDirectoryVariableName);
            if (!directoryUrl.has_value())
            {
                GTEST_SKIP() << "未设置 " << kDirectoryUrlVariableName << "，跳过真机构用例（协议本身已由进程内桩逐条判过）";
            }
            if (!contactEmail.has_value())
            {
                GTEST_SKIP() << "未设置 " << kContactEmailVariableName << "：注册账户等于替操作者接受服务条款，不代填";
            }
            if (!stateDirectory.has_value())
            {
                GTEST_SKIP() << "未设置 " << kAccountStateDirectoryVariableName << "：账户要跨轮次持久，否则每跑一次回归就在机构侧多出一个真账户";
            }

            m_directoryUrl   = std::move(*directoryUrl);
            m_contactEmail   = std::move(*contactEmail);
            m_stateDirectory = std::move(*stateDirectory);

            std::error_code ignore;
            std::filesystem::create_directories(m_stateDirectory, ignore);

            const std::filesystem::path keyFile = m_stateDirectory / kAccountKeyFileName;
            if (std::filesystem::exists(keyFile))
            {
                auto loaded = AcmeKeyPair::loadFromFile(keyFile);
                if (loaded.has_value())
                {
                    m_accountKey = std::make_unique<AcmeKeyPair>(std::move(*loaded));
                    return;
                }
                // 落盘的密钥读不回来时不悄悄换一把：换键就是换账户，那会在机构侧留下孤儿账户，
                // 而「第二轮复用同一个 URL」这条判据看起来仍然成立
                m_preparationFailure = loaded.error().message;
                return;
            }

            auto generated = AcmeKeyPair::generate(AcmeKeyAlgorithm::Rs256);
            if (!generated.has_value())
            {
                m_preparationFailure = generated.error().message;
                return;
            }
            auto saved = generated->saveToFile(keyFile);
            if (!saved.has_value())
            {
                m_preparationFailure = saved.error().message;
                return;
            }
            m_accountKey = std::make_unique<AcmeKeyPair>(std::move(*generated));
        }

        /// 跑两轮并交回各自的结局：循环只 run() 一次，两轮连着的驱动协程跑完才叫停
        Outcome runTwoRounds()
        {
            if (!m_preparationFailure.empty())
            {
                m_outcome.failureMessage = "账户密钥准备失败：" + m_preparationFailure;
                return m_outcome;
            }
            if (!m_accountKey)
            {
                m_outcome.failureMessage = "账户密钥没准备好（SetUp 未走到成功出口）";
                return m_outcome;
            }

            m_loop = std::make_unique<Core::EventLoop>();
            m_task.emplace(driveRounds());
            m_loop->scheduler().schedule(m_task->handle());
            m_loop->run();
            // 帧必须在循环析构之前销毁：循环析构会收掉还挂在上面的等待者
            m_task.reset();
            m_loop.reset();
            return m_outcome;
        }

        std::string           m_directoryUrl{};       ///< 机构目录
        std::string           m_contactEmail{};       ///< 注册用的 mailto: 地址
        std::filesystem::path m_stateDirectory{};     ///< 账户材料的持久目录
        std::string           m_preparationFailure{}; ///< 密钥准备失败的原因；非空即不联网
        Outcome               m_outcome{};            ///< 本轮结局，驱动协程按引用写它

    private:
        /**
         * @brief 驱动协程
         * @details 每条出口都叫停循环：漏一条就把整条用例挂在一条没人再推进的循环上。
         */
        Core::Task<void> driveRounds()
        {
            std::optional<std::string> persistedUrl = readPersistedAccountUrl();

            {
                AcmeClient client(*m_loop, makeConfiguration(), *m_accountKey);
                auto       prepared = co_await client.prepareAccount(persistedUrl);
                if (!prepared.has_value())
                {
                    m_outcome.failureMessage = "第一轮：" + prepared.error().message;
                    m_loop->stop();
                    co_return;
                }
                m_outcome.isFirstRoundValid    = true;
                m_outcome.firstRoundAccountUrl = std::string(client.accountUrl());
            }

            {
                // 第二轮刻意不带持久化的旧值，只认第一轮拿回来的那个 URL：
                // 复用的判据要落在「同一把密钥 + 同一个账户」上，而不是落在磁盘上的残值上
                AcmeClient client(*m_loop, makeConfiguration(), *m_accountKey);
                auto       prepared = co_await client.prepareAccount(m_outcome.firstRoundAccountUrl);
                if (!prepared.has_value())
                {
                    m_outcome.failureMessage = "第二轮：" + prepared.error().message;
                    m_loop->stop();
                    co_return;
                }
                m_outcome.isSecondRoundValid    = true;
                m_outcome.secondRoundAccountUrl = std::string(client.accountUrl());
            }

            // 账户 URL 只在两轮都成功之后落盘：半途失败就把指针写走，下一轮会拿着机构不认的 URL 起步
            std::string writeFailure;
            if (!Platform::AtomicFileWriter::writeText(m_stateDirectory / kAccountUrlFileName, m_outcome.secondRoundAccountUrl, std::nullopt, &writeFailure))
            {
                m_outcome.failureMessage = "账户 URL 落盘失败：" + writeFailure;
            }

            m_loop->stop();
            co_return;
        }

        /**
         * @brief 读回上一轮留下的账户 URL；没有文件或正文为空都算「没有」
         */
        [[nodiscard]] std::optional<std::string> readPersistedAccountUrl() const
        {
            const std::filesystem::path urlFile = m_stateDirectory / kAccountUrlFileName;
            std::error_code             failure;
            const std::uintmax_t        fileSize = std::filesystem::file_size(urlFile, failure);
            if (static_cast<bool>(failure) || fileSize == 0U)
            {
                return std::nullopt;
            }
            auto contents = Platform::readFileContents(urlFile, 0U, static_cast<std::size_t>(fileSize));
            if (!contents.has_value() || contents->empty())
            {
                return std::nullopt;
            }
            return std::move(*contents);
        }

        /**
         * @brief 拼一份真机构配置
         * @details isTermsOfServiceAccepted 恒为真是本用例的前提：SetUp 已要求操作者显式给出联系邮箱，
         *          那是「我读过并同意条款」的唯一入口，缺它就 SKIP 而不是替他同意。
         */
        [[nodiscard]] AcmeClient::Configuration makeConfiguration() const
        {
            AcmeClient::Configuration configuration;
            configuration.directoryUrl             = m_directoryUrl;
            configuration.contactEmailAddress      = m_contactEmail;
            configuration.isTermsOfServiceAccepted = true;
            configuration.requestTimeout           = kRequestTimeout;
            return configuration;
        }

        std::unique_ptr<Core::EventLoop> m_loop;       ///< 承载出站请求的循环，只在测试线程上跑
        std::unique_ptr<AcmeKeyPair>     m_accountKey; ///< 账户密钥，须活得比客户端久
        std::optional<Core::Task<void>>  m_task;       ///< 驱动协程的帧：必须活到循环停下之后
    };

    /**
     * @brief 钉住：真机构收下我们的 JWS，且第二轮走的是「复用已有账户」那条出口
     * @details 三层判据：① 两轮都成功（真裁判验签通过、目录字段读对、TLS 过了真信任库）；
     *          ② 两轮的账户 URL 逐字相同（复用而不是重复建号）；③ URL 非空且以 https 起步。
     *          第三条看着弱，守的是「Location 头没读回来时实现会不会拿空串继续往下走」。
     */
    TEST_F(AcmeLiveAuthorityTest, PreparesAndReusesAnAccountOnARealAuthority)
    {
        const Outcome outcome = runTwoRounds();

        ASSERT_TRUE(outcome.isFirstRoundValid) << outcome.failureMessage;
        ASSERT_TRUE(outcome.isSecondRoundValid) << outcome.failureMessage;

        EXPECT_FALSE(outcome.firstRoundAccountUrl.empty()) << "账户建成功了却没拿到账户 URL：Location 头没读回来";
        EXPECT_EQ(outcome.secondRoundAccountUrl, outcome.firstRoundAccountUrl) << "第二轮没复用第一轮那个账户——在机构侧又多出一个账户";
        EXPECT_EQ(outcome.firstRoundAccountUrl.rfind("https://", 0), 0U) << "真机构的账户 URL 不可能是明文 http：" << outcome.firstRoundAccountUrl;
    }
} // namespace AsynGyanis::Net
