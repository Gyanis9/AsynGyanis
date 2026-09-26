/**
 * @file TlsPolicy.h
 * @brief TLS 策略：版本区间、套件与曲线、信任库与校验深度、吊销列表、票据开关——服务端与出站客户端共用一份
 * @author Gyanis
 * @date 2026-09-26
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <openssl/ssl.h>

#include <optional>
#include <string>

namespace AsynGyanis::Core
{
    /**
     * @brief TLS 的可配置面：一份策略同时喂给服务端上下文与出站客户端上下文
     *
     * @details 为什么要收成一份类型：加固项此前写死在 `TlsContext::createHardenedContext()` 里，
     *          QUIC 服务端与出站客户端又各建各的 SSL_CTX，于是「改一处策略」要翻三处代码，而这三处
     *          迟早会漂（证书安装与 ALPN 已经是两份实现了）。本类型只描述**策略**，不描述身份
     *          （证书、私钥、票据密钥仍走各自的加载接口，它们是「这台服务是谁」而不是「怎么握手」）。
     *
     * @note 每个字段都留了「不设置」的取值，且不设置时行为与本类型出现之前**逐字相同**：
     *       默认构造的策略就是既有加固档位（最低 TLS 1.2、安全等级 2、那份显式排除弱算法的套件列表）。
     *       这样默认构造路径不会给现存部署带来任何握手变化，可配置性只加在不满意的那部分人身上。
     * @note 套件串与曲线名不预先校验：什么算合法由 OpenSSL 说了算（它随发行版配置变化），
     *       自己先解析一遍只会把它支持的写法判成非法。应用失败时抛出的消息带着那份原文与出错的那一项
     * @see TlsContext, applyTlsPolicy()
     */
    struct TlsPolicy
    {
        /**
         * @brief 协议版本档位，只认这两档
         * @details 更低的两档（TLS 1.0/1.1）由 RFC 8996 列为废弃，本框架不接受、也不给配置项：
         *          留一个「能把它调回去」的口子等于把服务端重新暴露给 BEAST 一类的已知攻击面
         */
        enum class ProtocolVersion
        {
            Tls1_2, ///< TLS 1.2
            Tls1_3, ///< TLS 1.3
        };

        /// 最低协议版本；空表示用本角色的默认（服务端/客户端 TLS 1.2，QUIC 只能 1.3）
        std::optional<ProtocolVersion> minimumProtocolVersion;
        /// 最高协议版本；空表示用 OpenSSL 支持到的那一档
        std::optional<ProtocolVersion> maximumProtocolVersion;

        /// TLS 1.2 及以下的套件串（OpenSSL 语法）；空表示用内置那份显式排除弱算法的列表
        std::string cipherList;
        /// TLS 1.3 套件串（OpenSSL 语法，冒号分隔）；空表示交给 OpenSSL 默认
        std::string tls13CipherSuites;
        /// 允许的命名曲线/组，冒号分隔（如 `X25519:secp384r1`）；空表示 OpenSSL 默认
        std::string supportedGroups;

        /// OpenSSL 安全等级；默认 2（拒 1024 位以下的 RSA/DH 与 SHA-1 签名）。0 表示只做最小防护
        int securityLevel{2};

        /// 校验对端证书用的 CA 文件（PEM，可含多张）；空表示不动信任库
        std::string certificateAuthorityFile;
        /// 校验对端证书用的 CA 目录（openssl c_rehash 过的哈希目录）；空表示不加载
        std::string certificateAuthorityPath;
        /// 证书链校验深度；空表示用 OpenSSL 默认（100 跳，实践中先被对端配置卡住）
        std::optional<int> verifyDepth;

        /**
         * @brief 吊销列表（CRL）文件；**给了就等于要求做吊销检查**
         *
         * @details 为什么把「开关」并进这一个字段：留一个独立的 requireRevocationChecking 布尔，
         *          就会出现「开了检查却没有任何列表可查」那种配置——它在 OpenSSL 侧不是「没查」而是
         *          **每条握手都失败**（unable to get certificate CRL）。挂在一个只能同时成立的动作上，
         *          这类配置就写不出来。
         * @note 只支持单个文件（PEM 或 DER，OpenSSL 自己分辨，也可以一份文件里放多张 CRL）。
         *       哈希目录那种布局刻意不做：多一处目录约定就多一处「c_rehash 忘了跑」这类只在运行期
         *       暴露的错，而把若干张 CRL 串成一个文件是同样可达、却不依赖部署动作的写法。
         * @note 吊销检查是**失败即关**的：启用它却没有覆盖到发证 CA 的列表时，握手一条也不成。
         *       这方向不是疏忽——「列表没同步到就被当作没吊销」比「同步断了先拒掉」危险。
         */
        std::string revocationListFile;
        /// 只查对端那一张证书，还是链上每张（含中间 CA）都查；前者是默认，也是常见的部署形态
        bool revocationCoversWholeChain{false};

        /// 是否启用会话票据。关掉它多用于「恢复只靠 session id 缓存」或合规要求；关掉后多进程间
        /// 也就无从共享恢复能力，票据密钥文件那条配置随之失去作用
        bool areSessionTicketsEnabled{true};
    };

    /**
     * @brief 把一份策略施加到已经建好的 SSL_CTX 上
     *
     * @details 加固项的先后是有讲究的：安全等级要先落地（它会连带影响套件可选范围），
     *          再设版本区间，最后才是套件与曲线——套件串在低安全等级下可能一个都匹配不上，
     *          顺序颠倒会得到一条「套件列表无效」的误导性报错。
     * @param context 目标上下文，所有权不归本函数（调用方保证它活得比策略应用久）
     * @param policy 待施加的策略
     * @param builtInCipherList 策略里套件串为空时使用的本角色默认；**传 nullptr 表示「策略没给就不设
     *        这一项」**（出站客户端正是这种：它原先不写套件列表，写了反而可能把本可以连上的服务器拒掉）
     * @throws CoreException 任一 OpenSSL 调用拒绝了这个值：消息点名是哪一项、原文是什么
     * @note 失败即抛，不做「这项没生效但其它照旧」的降级：半生效的 TLS 策略比启动失败危险得多
     */
    void applyTlsPolicy(SSL_CTX *context, const TlsPolicy &policy, const char *builtInCipherList);
}
