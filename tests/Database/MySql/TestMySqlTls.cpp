// MySQL 链路 TLS 的活体用例：档位映射到客户端库 ssl-mode 后的效果（Ssl_cipher 读数）与 CA 校验的两条去向。
// 门控与 TestMySqlIntegration.cpp 同一套环境变量（口令没有默认值，未设置整组跳过）；VerifyCa 正向用例
// 另需 ASYN_MYSQL_TEST_SSL_CA 指向服务端的 CA 文件，未提供时那条用例单独跳过。
// 判据口径：Ssl_cipher 是本会话的协商结果（SHOW SESSION STATUS），空 = 这条连接是明文。

#include "DatabaseTestSupport.h"

#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/MySql/MySqlConnection.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace AsynGyanis::Database
{
    namespace
    {
        constexpr const char   *kHostVariableName     = "ASYN_MYSQL_TEST_HOST";
        constexpr const char   *kPortVariableName     = "ASYN_MYSQL_TEST_PORT";
        constexpr const char   *kUserVariableName     = "ASYN_MYSQL_TEST_USER";
        constexpr const char   *kPasswordVariableName = "ASYN_MYSQL_TEST_PASSWORD";
        constexpr const char   *kSslCaVariableName    = "ASYN_MYSQL_TEST_SSL_CA";
        constexpr const char   *kDefaultHost          = "127.0.0.1";
        constexpr std::uint16_t kDefaultPort          = 3306U;
        constexpr const char   *kDefaultUserName      = "root";

        /// 仓库自签夹具证书：拿它当「错误的 CA」用（与服务端自签 CA 必然不同源）
        const std::filesystem::path kUntrustedCertificateFixture = std::filesystem::path(TEST_FIXTURES_DIR) / "test_cert.pem";

#ifdef DATABASE_HAS_MYSQL
        constexpr bool kMySqlDriverCompiled = true;
#else
        constexpr bool kMySqlDriverCompiled = false;
#endif

        /// 门控：驱动编译过且口令已给；不满足时把原因写进 skipReason 由用例体跳过
        [[nodiscard]] bool tlsGatingPasses(std::string &skipReason)
        {
            if (!kMySqlDriverCompiled)
            {
                skipReason = "当前构建未编译 MySQL 驱动（未定义 DATABASE_HAS_MYSQL），跳过真实服务端 TLS 用例";
                return false;
            }
            if (TestSupport::readEnvironmentVariableText(kPasswordVariableName).empty())
            {
                skipReason = std::string("未设置环境变量 ") + kPasswordVariableName + "，跳过真实 MySQL TLS 用例";
                return false;
            }
            return true;
        }

        /// 从环境装载一份连接配置；不选默认库（本用例只读会话状态，不碰任何库表）
        [[nodiscard]] ConnectionConfig connectionConfigFromEnvironment()
        {
            ConnectionConfig configuration = ConnectionConfig::mySqlDefault();
            configuration.host             = TestSupport::readEnvironmentTextOrDefault(kHostVariableName, kDefaultHost);
            configuration.port             = TestSupport::readEnvironmentPortOrDefault(kPortVariableName, kDefaultPort);
            configuration.userName         = TestSupport::readEnvironmentTextOrDefault(kUserVariableName, kDefaultUserName);
            configuration.password         = TestSupport::readEnvironmentVariableText(kPasswordVariableName);
            configuration.database.clear();
            return configuration;
        }

        /// 连上并按 `SHOW SESSION STATUS` 读本会话的 Ssl_cipher（连不上时交回空 + 原因）
        struct LinkEncryptionProbe
        {
            bool        isConnected{false};
            std::string sslCipher; ///< 空 = 明文连接
            std::string failureReason;
        };

        [[nodiscard]] LinkEncryptionProbe probeLinkEncryption(const ConnectionConfig &configuration)
        {
            LinkEncryptionProbe probe;
            MySqlConnection     connection(configuration);
            if (!connection.connect())
            {
                probe.failureReason = connection.lastError();
                return probe;
            }
            probe.isConnected = true;

            const std::unique_ptr<DatabaseResult> cipherRows = TestSupport::executeRequired(connection, "SHOW SESSION STATUS LIKE 'Ssl_cipher'");
            if (!cipherRows->next())
            {
                probe.failureReason = "SHOW SESSION STATUS 没有交出任何一行";
                return probe;
            }
            probe.sslCipher = TestSupport::asText(cipherRows->getValue(std::size_t{1})).value_or("");
            return probe;
        }
    } // namespace

    /**
     * @brief 显式 Disabled：绝不对链路加密——要么明文建成（cipher 为空），要么被服务端的安全姿态拒绝
     * @details MySQL 8 的 caching_sha2_password 在明文通道上拒绝口令认证（错误 2061）：这是服务端
     *          防止口令裸奔的设计，不是驱动故障。本用例把两种结局都钉住：无论如何不得出现密码套件
     */
    TEST(MySqlTls, DisabledNeverNegotiatesALinkCipher)
    {
        std::string skipReason;
        if (!tlsGatingPasses(skipReason))
        {
            GTEST_SKIP() << skipReason;
        }

        ConnectionConfig configuration = connectionConfigFromEnvironment();
        configuration.tls.mode         = DatabaseTlsPolicy::Mode::Disabled;

        const LinkEncryptionProbe probe = probeLinkEncryption(configuration);
        if (probe.isConnected)
        {
            EXPECT_TRUE(probe.sslCipher.empty()) << "Disabled 会话竟然谈出了密码套件：" << probe.sslCipher;
        } else
        {
            EXPECT_NE(probe.failureReason.find("secure connection"), std::string::npos) << "Disabled 的失败不是服务端的明文口令防护（2061），值得查：" << probe.failureReason;
        }
    }

    /**
     * @brief Required：链路必须加密，但不校验证书链（服务端自签证书也接受）
     */
    TEST(MySqlTls, RequiredEncryptsTheLinkWithoutChainVerification)
    {
        std::string skipReason;
        if (!tlsGatingPasses(skipReason))
        {
            GTEST_SKIP() << skipReason;
        }

        ConnectionConfig configuration = connectionConfigFromEnvironment();
        configuration.tls.mode         = DatabaseTlsPolicy::Mode::Required;

        const LinkEncryptionProbe probe = probeLinkEncryption(configuration);
        ASSERT_TRUE(probe.isConnected) << probe.failureReason;
        EXPECT_FALSE(probe.sslCipher.empty()) << "Required 会话没有谈出密码套件：链路是明文";
    }

    /**
     * @brief VerifyCa 正向：CA 与服务器自签 CA 同源时会话建成且加密
     */
    TEST(MySqlTls, VerifyCaAcceptsTheServerWhenTheCaFileMatches)
    {
        std::string skipReason;
        if (!tlsGatingPasses(skipReason))
        {
            GTEST_SKIP() << skipReason;
        }
        const std::string serverCaFile = TestSupport::readEnvironmentVariableText(kSslCaVariableName);
        if (serverCaFile.empty())
        {
            GTEST_SKIP() << "未设置环境变量 " << kSslCaVariableName << "（服务端 CA 文件路径），跳过 VerifyCa 正向用例";
        }

        ConnectionConfig configuration             = connectionConfigFromEnvironment();
        configuration.tls.mode                     = DatabaseTlsPolicy::Mode::VerifyCa;
        configuration.tls.certificateAuthorityFile = serverCaFile;

        const LinkEncryptionProbe probe = probeLinkEncryption(configuration);
        ASSERT_TRUE(probe.isConnected) << probe.failureReason;
        EXPECT_FALSE(probe.sslCipher.empty()) << "VerifyCa 会话没有谈出密码套件：链路是明文";
    }

    /**
     * @brief VerifyCa 负向：CA 与服务端不同源时必须在握手期失败（绝不回落明文）
     */
    TEST(MySqlTls, VerifyCaRejectsAnUntrustedCa)
    {
        std::string skipReason;
        if (!tlsGatingPasses(skipReason))
        {
            GTEST_SKIP() << skipReason;
        }

        ConnectionConfig configuration             = connectionConfigFromEnvironment();
        configuration.tls.mode                     = DatabaseTlsPolicy::Mode::VerifyCa;
        configuration.tls.certificateAuthorityFile = kUntrustedCertificateFixture.string();

        const LinkEncryptionProbe probe = probeLinkEncryption(configuration);
        EXPECT_FALSE(probe.isConnected) << "错误 CA 之下连接竟然建成了";
        EXPECT_FALSE(probe.failureReason.empty()) << "拒绝路径没有留下可读原因";
    }

    /**
     * @brief VerifyCa 缺 CA 是配置错误：连接期前置拒绝，原因是「缺信任锚」而不是底层库的含糊报错
     */
    TEST(MySqlTls, VerifyCaWithoutCaFailsBeforeDialing)
    {
        std::string skipReason;
        if (!tlsGatingPasses(skipReason))
        {
            GTEST_SKIP() << skipReason;
        }

        ConnectionConfig configuration = connectionConfigFromEnvironment();
        configuration.tls.mode         = DatabaseTlsPolicy::Mode::VerifyCa;

        const LinkEncryptionProbe probe = probeLinkEncryption(configuration);
        EXPECT_FALSE(probe.isConnected);
        EXPECT_NE(probe.failureReason.find("certificateAuthorityFile"), std::string::npos) << probe.failureReason;
    }
} // namespace AsynGyanis::Database
