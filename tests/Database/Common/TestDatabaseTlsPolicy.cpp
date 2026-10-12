// DatabaseTlsPolicy 校验的纯用例：档位与文件三路的自洽判据（无网络、无连接）。
#include "Database/Common/ConnectionConfig.h"

#include <gtest/gtest.h>

#include <string>

namespace AsynGyanis::Database
{
    /**
     * @brief VerifyCa / VerifyIdentity 缺信任锚必须当场拒绝（这两种模式没有 CA 就无从校验）
     */
    TEST(DatabaseTlsPolicy, VerifyModesRequireACertificateAuthorityFile)
    {
        DatabaseTlsPolicy verifyCa;
        verifyCa.mode             = DatabaseTlsPolicy::Mode::VerifyCa;
        const auto verifyCaReason = verifyCa.validate();
        ASSERT_TRUE(verifyCaReason.has_value()) << "VerifyCa 缺 CA 应当被拒";
        EXPECT_NE(verifyCaReason->find("certificateAuthorityFile"), std::string::npos) << *verifyCaReason;

        verifyCa.certificateAuthorityFile = "/tmp/ca.pem";
        EXPECT_FALSE(verifyCa.validate().has_value()) << "给了 CA 的 VerifyCa 应当合法";

        DatabaseTlsPolicy verifyIdentity;
        verifyIdentity.mode = DatabaseTlsPolicy::Mode::VerifyIdentity;
        ASSERT_TRUE(verifyIdentity.validate().has_value()) << "VerifyIdentity 缺 CA 应当被拒";
    }

    /**
     * @brief 客户端证书与私钥必须成对；Required / Preferred 无 CA 合法（不校链的两档）
     */
    TEST(DatabaseTlsPolicy, ClientIdentityNeedsBothFilesAndOptionalModesAreSelfConsistent)
    {
        DatabaseTlsPolicy onlyCertificate;
        onlyCertificate.mode                  = DatabaseTlsPolicy::Mode::Required;
        onlyCertificate.clientCertificateFile = "/tmp/client.pem";
        const auto pairReason                 = onlyCertificate.validate();
        ASSERT_TRUE(pairReason.has_value()) << "只给客户端证书不给私钥应当被拒";
        EXPECT_NE(pairReason->find("成对"), std::string::npos) << *pairReason;

        DatabaseTlsPolicy onlyKey;
        onlyKey.mode                 = DatabaseTlsPolicy::Mode::Preferred;
        onlyKey.clientPrivateKeyFile = "/tmp/client.key";
        ASSERT_TRUE(onlyKey.validate().has_value()) << "只给私钥不给客户端证书应当被拒";

        DatabaseTlsPolicy plainRequired;
        plainRequired.mode = DatabaseTlsPolicy::Mode::Required;
        EXPECT_FALSE(plainRequired.validate().has_value()) << "Required 不需要 CA，空配置应当合法";

        DatabaseTlsPolicy plainDisabled;
        EXPECT_FALSE(plainDisabled.validate().has_value()) << "默认档的空配置应当合法";
    }

    /**
     * @brief 三个文件路径交给 OpenSSL/客户端库时是零终止 C 字符串：内嵌 NUL 一律先拦
     */
    TEST(DatabaseTlsPolicy, RejectsEmbeddedNulInFilePaths)
    {
        DatabaseTlsPolicy nulCa;
        nulCa.mode                     = DatabaseTlsPolicy::Mode::VerifyCa;
        nulCa.certificateAuthorityFile = std::string("a\0b", 3);
        const auto nulReason           = nulCa.validate();
        ASSERT_TRUE(nulReason.has_value()) << "CA 路径含内嵌 NUL 应当被拒";
        EXPECT_NE(nulReason->find("NUL"), std::string::npos) << *nulReason;

        DatabaseTlsPolicy nulKey;
        nulKey.mode                  = DatabaseTlsPolicy::Mode::Required;
        nulKey.clientCertificateFile = "/tmp/client.pem";
        nulKey.clientPrivateKeyFile  = std::string("k\0y", 3);
        ASSERT_TRUE(nulKey.validate().has_value()) << "私钥路径含内嵌 NUL 应当被拒";
    }
} // namespace AsynGyanis::Database
