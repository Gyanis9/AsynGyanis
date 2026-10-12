/**
 * @file ConnectionConfig.h
 * @brief 数据库连接配置
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once

#include "AsynGyanisExport.h"

#include <cstdint>
#include <optional>
#include <string>

namespace AsynGyanis::Database
{
    /**
     * @brief 网络驱动的链路 TLS 策略（MySQL / Redis 共用；SQLite 是进程内文件库，配了即拒）
     *
     * @details 档位命名与 MySQL 客户端的 ssl-mode 一一对应，两条驱动因此同一套词表。
     *          默认 Preferred：与 MySQL 客户端库的既有默认档一致（服务器支持就加密、谈不成回落明文），
     *          对既有部署零行为变化；要不要加密从此由本配置显式决定，不再随客户端库的默认档漂移。
     *          `ConnectionConfig::redisDefault()` 例外地把默认落成 Disabled（对齐 Redis 生态「默认明文」），
     *          要加密在它之上显式选档即可。
     * @note Preferred 的语义两条驱动一致：先试 TLS，谈不成回落明文（连接与认证都从头来过）。
     */
    struct ASYN_DATABASE_API DatabaseTlsPolicy
    {
        /// 链路加密与证书校验的档位
        enum class Mode : std::uint8_t
        {
            Disabled = 0,  ///< 强制不加密；注意 MySQL 8 的 caching_sha2_password 在明文通道上会拒绝口令认证（错误 2061），那是服务端的安全姿态
            Preferred,     ///< 能加密就加密，谈不成回落到明文（默认档）
            Required,      ///< 必须加密；不校验证书链（对齐 MySQL ssl-mode 的 REQUIRED）
            VerifyCa,      ///< 必须加密且证书链要过 CA 校验（不校验主机名）
            VerifyIdentity ///< 在 VerifyCa 之上再校验主机名与证书相符
        };

        Mode        mode{Mode::Preferred};    ///< 档位；SQLite 上 Preferred 视同 Disabled（进程内库无链路可加密），Required 及两个校验档则被拒
        std::string certificateAuthorityFile; ///< CA 文件；VerifyCa 与 VerifyIdentity 必填
        std::string clientCertificateFile;    ///< 双向 TLS 的客户端证书；与私钥成对给出
        std::string clientPrivateKeyFile;     ///< 双向 TLS 的客户端私钥

        /**
         * @brief 校验这组配置自身是否自洽
         * @return std::optional<std::string> 不合法时给出可操作的中文原因；合法为空
         * @details 文件路径都是零终止 C 字符串交给 OpenSSL 与客户端库的，内嵌 NUL 会静默截断成
         *          半截路径——与 host 那几处同一口径，在本层先拦下
         */
        [[nodiscard]] std::optional<std::string> validate() const
        {
            // 路径先查 NUL：截断出的半截路径既可能找不到文件，也可能命中另一个文件，两种都难查
            const std::string_view pathTexts[] = {certificateAuthorityFile, clientCertificateFile, clientPrivateKeyFile};
            for (const std::string_view pathText: pathTexts)
            {
                if (pathText.find('\0') != std::string_view::npos)
                {
                    return "链路 TLS 策略的证书路径含内嵌 NUL 字节，OpenSSL 只接受零终止字符串并会静默截断——请检查配置的来源";
                }
            }

            // VerifyCa / VerifyIdentity 的全部判据就是「对照信任锚」，没有锚的这两个模式没有意义
            if ((mode == Mode::VerifyCa || mode == Mode::VerifyIdentity) && certificateAuthorityFile.empty())
            {
                return "链路 TLS 策略选了 VerifyCa / VerifyIdentity 却没有给 certificateAuthorityFile：没有信任锚时这两个模式无从校验";
            }

            // 成对给出是 TLS 客户端库的共同要求（单给一半的报错都指向实现细节，不如在这里点名）
            if (clientCertificateFile.empty() != clientPrivateKeyFile.empty())
            {
                return "链路 TLS 策略的客户端证书与私钥必须成对给出（clientCertificateFile / clientPrivateKeyFile）";
            }
            return std::nullopt;
        }
    };

    /**
     * @brief 档位名：错误文案与日志里点名用（与 MySQL ssl-mode 的拼写一致）
     * @param mode 档位
     * @return std::string_view 英文档位名
     */
    [[nodiscard]] inline std::string_view databaseTlsModeName(const DatabaseTlsPolicy::Mode mode) noexcept
    {
        switch (mode)
        {
            case DatabaseTlsPolicy::Mode::Disabled:
                return "Disabled";
            case DatabaseTlsPolicy::Mode::Preferred:
                return "Preferred";
            case DatabaseTlsPolicy::Mode::Required:
                return "Required";
            case DatabaseTlsPolicy::Mode::VerifyCa:
                return "VerifyCa";
            case DatabaseTlsPolicy::Mode::VerifyIdentity:
                return "VerifyIdentity";
        }
        return "Unknown";
    }

    /**
     * @brief 数据库连接配置
     *
     * @details 只保留各驱动确实会读取的字段。超时不在这里配置，由 DatabaseConnection 的
     *          setConnectTimeout / setQueryTimeout 管理，避免出现两处互相覆盖的真值来源。
     */
    struct ASYN_DATABASE_API ConnectionConfig
    {
        std::string       host;     ///< 主机地址；SQLite 忽略该字段
        std::uint16_t     port = 0; ///< 端口号；0 表示未指定，SQLite 不使用端口
        std::string       userName; ///< 用户名；SQLite 忽略
        std::string       password; ///< 密码；SQLite 忽略
        std::string       database; ///< 数据库名、Redis 键空间编号或 SQLite 文件路径
        DatabaseTlsPolicy tls;      ///< 链路 TLS 策略；MySQL 与 Redis 读取，SQLite 配了即拒

        /**
         * @brief 构造一份 MySQL 默认配置
         * @return ConnectionConfig 指向本机 3306 的 root/test 配置
         */
        static ConnectionConfig mySqlDefault()
        {
            ConnectionConfig configuration;
            configuration.host     = "127.0.0.1";
            configuration.port     = 3306;
            configuration.userName = "root";
            configuration.database = "test";
            return configuration;
        }

        /**
         * @brief 构造一份 Redis 默认配置
         * @return ConnectionConfig 指向本机 6379 的配置
         * @note 链路 TLS 默认 Disabled：对齐 Redis 生态「默认明文」，明文连接不付一次注定失败的
         *       TLS 试探；要加密在返回值上显式选档（Required / VerifyCa / VerifyIdentity 等）
         */
        static ConnectionConfig redisDefault()
        {
            ConnectionConfig configuration;
            configuration.host     = "127.0.0.1";
            configuration.port     = 6379;
            configuration.tls.mode = DatabaseTlsPolicy::Mode::Disabled;
            return configuration;
        }

        /**
         * @brief 构造一份 SQLite 配置
         * @param databasePath 数据库文件路径，传 ":memory:" 得到内存库
         * @return ConnectionConfig 仅填充 database 字段的配置
         * @note 链路 TLS 显式落 Disabled：进程内文件库没有可加密的网络链路（Preferred 在此视同 Disabled，
         *       而 Required 及两个校验档会被连接层拒——那才是「配错了」的形状）
         */
        static ConnectionConfig sqliteDefault(std::string databasePath = ":memory:")
        {
            ConnectionConfig configuration;
            configuration.database = std::move(databasePath);
            configuration.tls.mode = DatabaseTlsPolicy::Mode::Disabled;
            return configuration;
        }
    };

} // namespace AsynGyanis::Database
