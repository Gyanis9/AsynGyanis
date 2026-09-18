/**
* @file TlsContext.h
 * @brief SSL_CTX RAII 包装器 — 管理 TLS 服务端上下文及每个连接的 SSL 对象创建
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once

#include <openssl/err.h>

#include <mutex>
#include <string>

namespace AsynGyanis::Core
{
    /**
     * @brief SSL_CTX 的 RAII 包装器，管理 TLS 服务端上下文和每个连接的 SSL 对象创建
     * @note 构造即完成安全加固：最低 TLS 1.2、关闭压缩、安全等级 2、显式排除弱套件，
     *       ALPN 只从客户端提供过的名字里挑（偏好 h2）
     * @note 非 Windows 平台在构造时忽略 SIGPIPE：OpenSSL 的内部写入不走 MSG_NOSIGNAL，
     *       对端已关闭时（握手失败发 alert、SSL_shutdown 发 close_notify）默认会打死进程，
     *       忽略后写失败以 EPIPE 返回，走既有错误路径
     * @note 配置挂在 SSL_CTX 上且被进程内所有连接共享：改动只影响之后创建的 SSL 对象，
     *       已建立的连接不受影响
     * @note 会话恢复按 OpenSSL 默认即开启；票据密钥随上下文生成，reloadCertificate() 换代后
     *       旧票据无法恢复，客户端自动退回全量握手
     * @note 证书换代不中断服务：reloadCertificate() 用同一套加固配置新建 SSL_CTX 整台换掉，
     *       已建立的连接仍绑在旧上下文上（OpenSSL 引用计数保证最后一个引用消失前不释放它）
     */
    class TlsContext
    {
    public:
        /**
         * @brief 构造 TlsContext 对象并创建 SSL_CTX 实例
         * @details 有意不做显式的库初始化：OpenSSL 1.1 起会自动初始化，SSL_library_init()
         *          之类的旧接口不必也不该由库代码替使用者调用
         * @throws CoreException 创建 SSL_CTX 失败（通常是内存不足），或安全加固项不被当前
         *         OpenSSL 支持（最低版本、套件列表）。它派生自 Base::Exception，一条 catch 可兜住
         */
        TlsContext();

        /**
         * @brief 析构函数，释放 SSL_CTX 资源（内部调用 SSL_CTX_free）。
         */
        ~TlsContext();

        TlsContext(const TlsContext &) = delete;

        TlsContext &operator=(const TlsContext &) = delete;

        /**
         * @brief 加载服务器证书和私钥文件。
         * @param certificateFile 证书文件路径（PEM 格式，通常包含证书链）
         * @param keyFile  私钥文件路径（PEM 格式）
         * @return 成功返回 true，失败返回 false（可通过 OpenSSL 错误栈获取日志）
         */
        bool loadCertificate(const std::string &certificateFile, const std::string &keyFile) const;

        /**
         * @brief 加载用于校验对端（客户端）证书的 CA 文件
         * @param caFile CA 文件路径（PEM，即客户端证书的签发者或其根）
         * @return 加载成功与否；false 时不做任何降级，原因留在 OpenSSL 错误栈里
         * @note 必须在任何 SSL 对象创建之前调用：信任库挂在 SSL_CTX 上，createSSL() 建好的
         *       SSL 不会看到本次变更；文件里可有多个证书（链搜索逐级找签发者）
         * @see setClientCertificateRequired()
         */
        bool loadClientCertificateAuthority(const std::string &caFile) const;

        /**
         * @brief 设置服务端是否要求客户端出示证书（mTLS）
         * @param required true 要求并校验客户端证书（不出示即终止握手，不退化成可选校验），
         *        false 关闭该校验
         * @throws CoreException 传 true 但此前从未成功加载过 CA：要求校验却没有 CA 会让每条
         *         连接都握手失败，属于配置错误，故当场拒绝
         * @note 必须在任何 SSL 对象创建之前调用：校验模式挂在 SSL_CTX 上，createSSL() 建好的
         *       SSL 不会看到本次变更
         * @see loadClientCertificateAuthority()
         */
        void setClientCertificateRequired(bool required) const;

        /**
         * @brief 为已建立的连接创建一个新的 SSL 对象。
         * @param fileDescriptor 已连接的 socket 文件描述符（用于 SSL_set_fd 设置底层描述符）
         * @return SSL 对象指针，所有权转移给调用者（通常由 TlsSocket 持有）；
         * @throws CoreException SSL_new 返回空，或 SSL_set_fd 绑定失败（文件描述符已关闭
         *         或不是套接字）。本方法**不返回 nullptr**：失败一律抛出，且绑定失败时
         *         已释放刚创建的 SSL 对象，不会泄漏
         */
        SSL *createSSL(int fileDescriptor) const;

        /**
         * @brief 获取底层的 SSL_CTX 原生句柄。
         * @return SSL_CTX* 指针
         */
        [[nodiscard]] SSL_CTX *nativeHandle() const;

        /**
         * @brief 用上次 loadCertificate() 记下的路径重新加载证书与私钥，成功则整台换用新上下文
         * @return true 新证书已生效（此后新建的连接用它）；false 表示没有可轮换的证书或新证书
         *         加载失败，此时旧证书原样继续服务
         * @note 有意不带参数：续期的标准流程是「新证书覆盖到原路径」（certbot 与 ACME 客户端都
         *       这么做），复用上次记下的路径可避免调用方传错路径而悄悄换掉身份
         * @note 先在**新上下文**上把加固与已加载的 CA、对端校验开关、OCSP 响应原样复现完，成功
         *       之后才整台换掉；任一步失败都不碰旧上下文（漏掉对端校验会让一次续期悄悄关掉 mTLS）
         * @note 与 createSSL() 互斥：OpenSSL 不允许在其它线程正用它创建 SSL 时改同一个 SSL_CTX，
         *       因此换代粒度是整台上下文，而不是就地改写
         * @note 已建立的连接不受影响：它们各自的 SSL 对象持有旧上下文的引用，最后一个引用消失前
         *       旧上下文不会被释放
         * @see loadCertificate()
         */
        bool reloadCertificate();

        /**
         * @brief 加载 OCSP 响应文件（DER 格式），此后握手按客户端请求装订（stapling）
         * @param ocspResponseFile OCSP 响应文件路径（DER；通常由 ACME 客户端随证书一并产出）
         * @return 文件不可读、内容为空或不是合法 DER 编码都返回 false，且不做任何变更；
         *         成功则此后新建的 SSL 会按需装订
         * @note 与证书同为「路径即身份」的续期形态：reloadCertificate() 按原路径重读，与证书一起换
         * @note 可在服务运行中调用：数据按上下文存储、读取侧无锁，已建立的连接不受影响
         * @note OpenSSL 3.x 只对非自签名的叶证书装订，并按响应中的序列号与签发者名哈希匹配当前
         *       证书；自签名部署下不会装订，这是 OpenSSL 的策略而非本类能绕过的
         */
        bool loadOcspResponse(const std::string &ocspResponseFile) const;

    private:
        /**
         * @brief 新建一个 SSL_CTX 并施加全部安全加固（构造与热轮换共用同一份，避免两处配置各自漂移）
         * @return SSL_CTX* 已加固的上下文，所有权归调用方
         * @throws CoreException 创建失败或加固项无法生效（调用方负责先释放已建的句柄）
         */
        [[nodiscard]] static SSL_CTX *createHardenedContext();

        /**
         * @brief 把证书与私钥装进指定上下文并校验两者配对
         * @param context 目标上下文
         * @param certificateFile 证书文件路径（PEM）
         * @param keyFile 私钥文件路径（PEM）
         * @return true 装好且配对；false 任一环节失败（原因留在 OpenSSL 错误栈里）
         */
        [[nodiscard]] static bool installCertificate(SSL_CTX *context, const std::string &certificateFile,
                                                    const std::string &keyFile);

        SSL_CTX *m_context{nullptr}; ///< OpenSSL SSL_CTX 句柄，RAII 管理
        mutable std::mutex m_contextMutex; ///< 保护 m_context 的读取与整台换代（createSSL/reload 互斥）

        // 下面四项记录「当前生效的配置」，供 reloadCertificate() 在新上下文上原样复现。
        // 加载类接口都是 const（它们改的是 SSL_CTX 内容而不是本对象的身份），因此这几项为 mutable
        mutable std::string m_certificateFile; ///< 上次成功加载的证书路径；空表示还没加载过，reloadCertificate() 据此判断
        mutable std::string m_keyFile;         ///< 上次成功加载的私钥路径
        mutable std::string m_clientCertificateAuthorityFile; ///< 已加载的校验 CA 路径；换代时要复现，空表示没加载过
        mutable std::string m_ocspResponseFile; ///< 已加载的 OCSP 响应路径；换代时按此重读，空表示没加载过

        mutable bool m_clientCertificateRequired{false};        ///< 是否要求并校验对端证书（换代时同样要复现）
        mutable bool m_clientCertificateAuthorityLoaded{false}; ///< 是否已成功加载校验对端证书的 CA
    };
} // namespace AsynGyanis::Core
