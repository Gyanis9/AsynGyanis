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
     * @brief SSL_CTX 的 RAII 包装器，管理 TLS 服务端上下文和每个连接的 SSL 对象创建。
     *
     * 封装 OpenSSL SSL_CTX 的生命周期（创建、配置、加载证书/私钥），
     * 并提供创建 SSL 对象的方法。析构时自动释放 SSL_CTX 资源。
     * 适用于服务端 TLS 连接的上下文管理。
     *
     * @details 构造即完成安全加固：最低 TLS 1.2、关闭压缩、安全等级 2 与显式排除弱套件、
     *          ALPN 支持 h2 与 http/1.1（只能从客户端提供过的名字里挑，偏好 h2）。
     *          需要 mTLS 时再调用 loadClientCertificateAuthority()
     *          与 setClientCertificateRequired()，两者都必须早于 createSSL()。
     *          非 Windows 平台还会在构造时忽略 SIGPIPE：OpenSSL 内部的写入不走 MSG_NOSIGNAL
     *          路径，对端已关闭时（握手失败发 alert、SSL_shutdown 发 close_notify）默认会触发
     *          SIGPIPE 打死进程；忽略后写失败以 EPIPE 返回，由现有错误路径处理。
     * @note 一个 SSL_CTX 被同一服务器进程内的所有连接共享，因此上述配置是全局生效的：
     *       改动只影响之后创建的 SSL 对象，已建立的连接不受影响。
     * @note 证书可以在不中断服务的前提下换代：reloadCertificate() 会用同一套加固配置新建一个
     *       SSL_CTX 并整台换掉，已建立的连接仍绑在旧上下文上（OpenSSL 的引用计数保证旧上下文
     *       在最后一个引用消失前不会被释放），因此续期不再需要重启进程。
     */
    class TlsContext
    {
    public:
        /**
         * @brief 构造 TlsContext 对象并创建 SSL_CTX 实例。
         *
         * @details 不做显式的库初始化：OpenSSL 1.1 起 SSL 库会自动初始化，
         *          SSL_library_init() / SSL_load_error_strings() 之类的旧接口已不再需要
         *          （也不该在库代码里替使用者调用）。创建的 SSL_CTX 使用 TLS_server_method()。
         * @throws CoreException 创建 SSL_CTX 失败（SSL_CTX_new 返回空，通常是内存不足或
         *         OpenSSL 未正确初始化），或安全加固项无法生效（最低版本、套件列表不被当前
         *         OpenSSL 支持）。它派生自 Base::Exception，调用方可用一条 catch 兜住
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
         * @brief 加载用于校验对端（客户端）证书的 CA 文件。
         * @details 证书进入 SSL_CTX 的信任库，仅当 setClientCertificateRequired(true) 后参与
         *          对端证书的链校验；文件里可以有多个证书（用链搜索逐级找签发者）。
         * @param caFile CA 文件路径（PEM 格式，即客户端证书的签发者或其根）
         * @return true 加载成功
         * @return false 加载失败（文件不存在、格式非法或没有可用证书），失败时不做任何降级；
         *         原因留在 OpenSSL 错误栈里，可用 ERR_get_error()/ERR_error_string_n() 取出
         * @note 必须在任何 SSL 对象创建之前调用：校验用的信任库挂在 SSL_CTX 上，
         *       createSSL() 建好的 SSL 不会看到本次变更
         * @see setClientCertificateRequired()
         */
        bool loadClientCertificateAuthority(const std::string &caFile) const;

        /**
         * @brief 设置服务端是否要求客户端出示证书（mTLS）。
         * @details 传 true 时把校验模式置为 SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT：
         *          对端必须出示证书，且该证书要能被已加载的 CA 验通，否则握手终止；
         *          传 false 时回到 SSL_VERIFY_NONE，既不要求也不校验对端证书。
         * @param required true 要求并校验客户端证书，false 关闭该校验
         * @throws CoreException 传 true 但此前从未成功加载过 CA：要求校验却没有 CA 会让
         *         每条连接都握手失败，属于配置错误，故在此直接拒绝
         * @note 必须在任何 SSL 对象创建之前调用：校验模式挂在 SSL_CTX 上，
         *       createSSL() 建好的 SSL 不会看到本次变更
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
         * @brief 用上次 loadCertificate() 记下的路径重新加载证书与私钥，成功则整台换用新上下文。
         *
         * @details 续期的标准流程是「新证书覆盖到原路径」——ACM 客户端与 certbot 都这么做，路径不变；
         *          因此本方法不带参数，直接复用上次记住的路径，避免调用方传错路径而悄悄换掉身份
         *          （那属于重新配置，请用 loadCertificate() 并自行安排写窗口）。
         *
         *          实现上是**先在新上下文上把整台配置重建完，成功之后才换**：新上下文同样经过构造期
         *          那套加固（最低版本、套件、ALPN、压缩开关），并且复现已加载的 CA 与对端校验开关——
         *          漏掉后者会让一次续期悄悄把 mTLS 关掉。任一步失败都返回 false 且**不碰旧上下文**，
         *          正在服务的连接与后续新连接都继续按旧证书走，不会因为一次轮换失败而掉线。
         *
         * @return true 新证书已生效（此后新建的连接用它）；false 没有可轮换的证书（尚未 loadCertificate()）
         *         或新证书加载失败，此时旧证书原样继续服务
         * @note 已建立的连接不受影响：它们各自的 SSL 对象持有旧上下文的引用，OpenSSL 在最后一个引用
         *       消失前不会释放它，因此握手中的连接与已通连的续传都照旧
         * @note 线程安全：与 createSSL() 互斥。换代的粒度是「整台上下文」，不做就地改写——
         *       OpenSSL 不允许在其它线程正用它创建 SSL 时改同一个 SSL_CTX
         * @see loadCertificate(), HttpsServer::reloadCertificate()
         */
        bool reloadCertificate();

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

        mutable bool m_clientCertificateRequired{false};        ///< 是否要求并校验对端证书（换代时同样要复现）
        mutable bool m_clientCertificateAuthorityLoaded{false}; ///< 是否已成功加载校验对端证书的 CA
    };
} // namespace AsynGyanis::Core
