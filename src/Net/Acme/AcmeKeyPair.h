/**
 * @file AcmeKeyPair.h
 * @brief ACME 用的密钥：JWK 表示、公钥指纹、JWS 签名与证书签名请求
 * @author Gyanis
 * @date 2026-09-28
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Net/Acme/AcmeError.h"

#include <openssl/types.h>

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    /// 本模块内部的 OpenSSL 句柄处置器（放在头文件里是因为 AcmeKeyPair 的成员类型要用它，
    /// 而实现里的自由函数也要用同一份——私有嵌套类型两边都够不着）
    namespace Detail
    {
        /**
         * @brief EVP_PKEY 的归还动作
         */
        struct ASYN_NET_API EvpKeyDeleter
        {
            /**
             * @brief 放掉一把 OpenSSL 密钥的引用
             * @param key 待归还的句柄；空指针按 OpenSSL 语义直接忽略
             */
            void operator()(EVP_PKEY *key) const noexcept;
        };

        /// 一把密钥的公开表示：JWK 紧凑文本与它的 RFC 7638 指纹，成对算出来以免两处漂移
        struct ASYN_NET_API PublicIdentity
        {
            std::string jsonWebKeyText;       ///< 只含公开成员、按键字典序排列的紧凑 JSON
            std::string jsonWebKeyThumbprint; ///< 上面那份文本的 SHA-256 指纹（base64url 无填充）
        };
    } // namespace Detail

    /**
     * @brief 一把 ACME 密钥用哪种 JWA 算法签名（RFC 7518）
     * @details 名字照 JWA 的标识符写（Rs256/Es256），不改写成 PascalCase 全拼：那几个串就是
     *          JWS 头部 "alg" 字段在线上的取值，与本类的算法名映射一一对得上。
     * @warning 颁发机构必须支持所选算法。RFC 8555 §6.2 只强制机构支持 RS256，ES256 是可选实现——
     *          因此默认档是 RS256；对着只支持 RS256 的机构选 ES256，会得到一条「算法不支持」的
     *          账户注册失败，而不是任何降级。
     */
    enum class AcmeKeyAlgorithm
    {
        Rs256, ///< RSA 2048 位 + SHA-256，机构侧唯一强制要支持的算法
        Es256, ///< NIST P-256（prime256v1）+ SHA-256，签名更短、握手更快
    };

    /**
     * @brief ACME 通路里的一把密钥：账户密钥与域名密钥都用它
     *
     * @details 一个类型顶两处用途是有意的：账户密钥（签 JWS）与域名密钥（进 CSR、随后守私钥）
     *          需要的能力集合完全相同——生成、落盘、按 JWK 交代公钥——分成两类就是两份 OpenSSL
     *          包装迟早会漂。
     *
     * @note 密钥材料留在内存里（EVP_PKEY 句柄），落盘走 PEM 私钥、不加密码：保护它的是文件权限
     *       （owner-rw，0600）而不是口令，与既有 TLS 私钥文件的部署形态一致。
     * @note 生成与签名都在调用线程上算：RSA 2048 生成约几十到几百毫秒、一次续期只有一回，
     *       落在事件循环线程上是可接受的短时计算（不是同步 IO 等待）。要把它挪出循环请用上
     *       Core::AsyncExecutor，本类不做——那会让「谁的线程上算」这件事变成两份契约。
     */
    class ASYN_NET_API AcmeKeyPair
    {
    public:
        /**
         * @brief 析构函数，释放底层的 EVP_PKEY（OpenSSL 引用归零时回收密钥材料）
         */
        ~AcmeKeyPair();

        AcmeKeyPair(const AcmeKeyPair &) = delete;

        AcmeKeyPair &operator=(const AcmeKeyPair &) = delete;

        AcmeKeyPair(AcmeKeyPair &&) noexcept;

        AcmeKeyPair &operator=(AcmeKeyPair &&) noexcept;

        /**
         * @brief 新生成一把密钥
         * @param algorithm 签名算法，见 AcmeKeyAlgorithm 的取舍说明
         * @return std::expected<AcmeKeyPair, AcmeError> 带好公钥指纹的密钥；OpenSSL 拒绝生成时
         *         交回 KeyMaterial 一档（通常只有内存不足一种成因）
         * @note 无静态工厂可用，故与 loadFromFile() 成对：构造私有一律走这两条入口，
         *       对象一旦存在就一定带一把可用的密钥与算好的指纹
         */
        [[nodiscard]] static std::expected<AcmeKeyPair, AcmeError> generate(AcmeKeyAlgorithm algorithm);

        /**
         * @brief 从 PEM 私钥文件读入一把密钥
         * @param privateKeyFile 私钥文件路径（PKCS#8 或传统 PEM，OpenSSL 自己分辨）
         * @return std::expected<AcmeKeyPair, AcmeError> 读出的密钥；算法由文件内容决定
         *         （RSA 密钥即 RS256、P-256 密钥即 ES256），不由调用方指定，因此不会出现
         *         「按 ES256 声明、实际是 RSA」这种线上凭据对不上的状态
         * @note 其它曲线（P-384 等）、其它位数的 RSA、加密私钥与公钥文件都判失败并写明原因：
         *       本类的签名与 JWK 序列化只按这两档写，收下来就是静默变形
         */
        [[nodiscard]] static std::expected<AcmeKeyPair, AcmeError> loadFromFile(const std::filesystem::path &privateKeyFile);

        /**
         * @brief 把私钥以 PEM 形式原子写入文件，并收成属主可读写
         * @param privateKeyFile 目标路径，父目录不存在时由原子写工具建出来
         * @return std::expected<void, AcmeError> 失败时交回 FileSystem 一档并带上底层原因
         * @note 权限 0600 只在 POSIX 上真正生效，Windows 侧只有只读位有意义（Platform 封装的既有
         *       口径），因此 Windows 部署要靠 ACL 或存放目录来守这把私钥
         * @note 已存在的目标文件会被原子替换：中途失败时旧文件内容不动，不会出现「私钥半个」的状态
         */
        [[nodiscard]] std::expected<void, AcmeError> saveToFile(const std::filesystem::path &privateKeyFile) const;

        /**
         * @brief 这把手上公钥的 JWK 表示（RFC 7517 §4 + RFC 7518 §6），只含公开成员
         * @details RS256 交回 {"e","kty","n"}，ES256 交回 {"crv","kty","x","y"}；私钥成员一个都不带。
         *          账户注册时这份文本要整段放进 JWS 头部（RFC 8555 §6.3），此后按账户 URL 引用（kid）
         *          就不再需要它。
         * @return std::string 紧凑 JSON（无空白），可直接拼进 JWS 头部
         */
        [[nodiscard]] std::string publicJsonWebKeyText() const;

        /**
         * @brief 公钥指纹（RFC 7638 §3：SHA-256 覆盖「只含必需公开成员、按键字典序排列」的紧凑 JSON）
         * @details 在构造时算好并留着：HTTP-01 的 keyAuthorization 每条挑战都要用它，而同一把账户
         *          密钥的指纹是常量。
         * @return const std::string & base64url 无填充的指纹文本
         */
        [[nodiscard]] const std::string &jsonWebKeyThumbprint() const noexcept;

        /**
         * @brief 对「protected.payload」这段待签串做 JWS 签名
         * @details ES256 要额外把 OpenSSL 交回的 DER 序列（ECDSA-Sig-Value）拆成定长 R‖S 各 32 字节
         *          再编码：JWA 的 ES256 线上格式就是裸 R‖S，写 DER 出去机构会判签名无效
         *          （RFC 7518 §3.4）。RS256 是 PKCS#1 v1.5，本身就是定长字节串，直接编码。
         * @param signingInput 待签的 ASCII 原文（本函数自己算摘要，调用方不要先哈希）
         * @return std::expected<std::string, AcmeError> base64url 无填充的签名段；
         *         失败交回 KeyMaterial 一档并写明是 OpenSSL 的哪一步
         */
        [[nodiscard]] std::expected<std::string, AcmeError> signJsonWebSigningInput(std::string_view signingInput) const;

        /**
         * @brief 为这批域名产出一份证书签名请求（PKCS#10）
         * @details 主题名取第一个域名做可读性便利，但**真正的凭据靠 SAN 扩展**：现代校验一律看
         *          subjectAltName，CN 不参与匹配（RFC 6125 已被 RFC 9525 取代，CA/Browser 基线也
         *          明确忽略 CN）。全部域名都写进 DNS 类型的 SAN。
         * @param domainNames 要覆盖的域名，至少一条；空列表直接拒绝而不是交出一张没有 SAN 的请求
         * @return std::expected<std::string, AcmeError> base64url 无填充的 DER 编码 CSR，
         *         即 ACME order finalize 要的 "csr" 字段取值
         * @note 请求用本密钥自己签名（challengePassword 一类附加属性一概不带）
         */
        [[nodiscard]] std::expected<std::string, AcmeError> createCertificateSigningRequest(const std::vector<std::string> &domainNames) const;

        /**
         * @brief 按 RFC 7638 的口径算一份 JWK 文本的指纹：SHA-256 后 base64url 无填充
         * @param canonicalJwkMembers 紧凑 JSON 原文，成员必须已按 ASCII 键的字典序排好且只含必需公开成员
         * @return std::string 指纹文本
         * @details 单独开成静态入口有两个用处：一是它能拿 RFC 的官方向量单独验收（不必持有一把
         *          与该向量对应的私钥），二是账户密钥之外的场景（比如核对机构侧记录的指纹）也要用同一份判据。
         * @note 本函数**不重排成员、不校验成员集合**：规范化那一步由 publicJsonWebKeyText() 保证，
         *       把任意 JSON 传进来算出的指纹与规范的 JWK 指纹不是同一件事，这是调用方的责任
         */
        [[nodiscard]] static std::string computeJsonWebKeyThumbprint(std::string_view canonicalJwkMembers);

        /**
         * @brief 本密钥对应的 JWA 算法名（"RS256" 或 "ES256"），即 JWS 头部 "alg" 的取值
         * @return std::string_view 指向静态字符串的视图
         */
        [[nodiscard]] std::string_view jsonWebAlgorithmName() const noexcept;

    private:
        /**
         * @brief 接管一把已存在的密钥并带上算好的公开表示
         * @param key 已创建好的 EVP_PKEY，所有权交给本对象
         * @param algorithm 由密钥内容推出来的算法
         * @param identity 公钥的 JWK 文本与指纹
         * @details 私有的构造只由 generate() 与 loadFromFile() 走：两者都先把密钥内容与算法核对过
         *          （曲线、位数）才算出公开表示，因此本对象一旦存在，`jsonWebAlgorithmName()` 与
         *          手里的密钥必然对得上
         */
        AcmeKeyPair(std::unique_ptr<EVP_PKEY, Detail::EvpKeyDeleter> key, AcmeKeyAlgorithm algorithm, Detail::PublicIdentity identity);

        std::unique_ptr<EVP_PKEY, Detail::EvpKeyDeleter> m_key;                  ///< 密钥本体，含私钥材料
        AcmeKeyAlgorithm                                 m_algorithm;            ///< 签名算法，与 m_key 的内容一致
        std::string                                      m_jsonWebKeyThumbprint; ///< 公钥指纹，构造时算好（RFC 7638）
        std::string                                      m_publicJsonWebKeyText; ///< 公钥 JWK 紧凑文本，构造时算好
    };
} // namespace AsynGyanis::Net
