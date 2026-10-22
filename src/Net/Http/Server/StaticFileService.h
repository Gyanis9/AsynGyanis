/**
 * @file StaticFileService.h
 * @brief 静态文件目录这项配置的本体与它的兜底路由登记：明文 HTTP、HTTPS 与 HTTP/3 三条服务器共用
 * @author Gyanis
 * @date 2026-09-26
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/Crypto/Digest.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace AsynGyanis::Net
{
    class Router;
    class StaticFileMappingCache;

    /**
     * @brief RFC 9842 的压缩字典登记：一本字典一份，哈希是配置时算好的正文 SHA-256
     *
     * @details 两个消费点都读这里：字典文件自身的响应带上 `Use-As-Dictionary: match="..."` 供客户端
     *          缓存，之后客户端按 `Available-Dictionary` 回送哈希，服务端与这里的 contentHash 逐字节
     *          比对决定能不能发 `.dcz` 副本。哈希在配置期算好并冻结——运行期每请求读字典文件
     *          重算，等于把一次整文件读加进请求路径。
     */
    struct ASYN_NET_API StaticFileCompressionDictionary
    {
        std::filesystem::path     path;          ///< 字典文件在静态根内的绝对路径（配置时解析、规范化）
        std::string               matchPattern;  ///< Use-As-Dictionary 的 match 取值（"/" 开头的路径模式）
        Core::Digest::Sha256Value contentHash{}; ///< 配置时读文件算出的正文 SHA-256（与客户端哈希比对）
    };

    /**
     * @brief 静态文件服务的运行时配置：开关、规范化后的根目录、Cache-Control 与映射缓存
     *
     * @details 由兜底路由的处理函数按 shared_ptr 只读持有，服务器一侧改的就是这个对象里的字段。
     *          按值捕获一份共享指针比捕获 `this` 更安全——处理函数可能在服务器之后才被销毁。
     * @note 只在事件循环线程（或启动前的配置期）读写：静态服务开关与目录的变更不会与在途请求交错。
     */
    struct ASYN_NET_API StaticFileSettings
    {
        bool                       isEnabled{false}; ///< 是否启用静态文件服务；根目录规范化失败即为 false
        std::filesystem::path      rootDirectory;    ///< 规范化（weakly_canonical）之后的静态根目录，绝对路径
        std::optional<std::string> cacheControl;     ///< 静态文件响应的 Cache-Control 值；空表示不发这条头
        /// 请求打到目录本身时是否生成 HTML 列表；false（默认）按 404 处理
        /**
         * @note 默认关闭是刻意的：列表会把目录结构、文件名与大小一并交给任何人来一句 `GET /assets/`
         *       的探测者，而多数部署里静态目录只是资源仓库，不是给人翻的档案柜。
         */
        bool listingEnabled{false};
        /**
         * @brief 是否按 `Accept-Encoding` 挑选同目录的预压缩副本（`.zst` / `.br` / `.gz`）作为表示
         * @details false（默认）时静态服务与加这一格之前逐字相同：即使目录里躺着 `app.js.br`，
         *          发出去的仍是 `app.js`。打开后按服务器偏好（见 `Detail::kCompressionCodecs` 的顺序）
         *          挑第一个「对端接受且磁盘上有副本」的算法，正文就是那份副本的字节，并如实带
         *          `Content-Encoding` 与 `Vary: Accept-Encoding`。
         * @note 默认关闭对齐业界惯例（nginx 的 `gzip_static` 也是 off）：它会改变同一 URL 的响应字节，
         *       而部署里叫 `.gz` 的同名文件未必是表示副本——可能是别人要原样下载的文件。
         * @note 代价是每请求多一次存在性探测（按偏好顺序试到命中为止，最多每算法一次），
         *       这份探测只在开着这一格时发生
         */
        bool precompressedVariantsEnabled{false};
        /**
         * @brief 映射缓存，由 StaticFileService::install() 按当时的限额建立，之后只读
         * @note 条目上限取自登记那一刻的 HttpServerLimits::maximumMappedStaticFiles，
         *       因此要改上限必须先 setLimits() 再设静态目录
         */
        std::shared_ptr<StaticFileMappingCache> mappingCache;

        /**
         * @brief RFC 9842 的压缩字典；空表示未配置
         * @note 生效面比其余各格窄：只有「变体协商开着 + 连接是 TLS + 客户端哈希匹配」三条同时成立
         *       时它才改变响应的形状（见 setCompressionDictionary 的说明）
         */
        std::optional<StaticFileCompressionDictionary> dictionary;
    };

    /**
     * @brief 「静态文件目录」配置的本体：把配置动作与那条 `any("*")` 兜底路由的登记收在一处
     *
     * @details 存在的理由：静态服务原先只长在 HttpServer 上，配置动作、目录规范化、Cache-Control 的
     *          合法性判定与兜底路由的登记全写死在它的方法里。HTTPS 与 h3 要接同一能力，只有两条路——
     *          把那一整段抄第二份、第三份（漂移的表现是「明文侧修了一个路径穿越，TLS 侧没有」这种
     *          最贵的一类），或者把本体收出来。这里收的是本体，各服务器一侧只留转发，语义逐字相同。
     * @note 成员函数的定义在 HttpServer.cpp：处理函数 serveStaticFileRequest 与它要用的路径清洗、
     *       条件请求判定都在那，搬来搬去只会把一处实现拆成两处读。
     * @warning install() 之前不能调其余配置方法（那是各服务器的 ensure 转发负责的事）。
     * @see StaticFileSettings, HttpServer::staticFileDir()
     */
    class ASYN_NET_API StaticFileService
    {
    public:
        /**
         * @brief 建立配置本体，并在路由器上登记那条兜底路由
         * @param router 要登记的路由器（本服务不持有它）
         * @param maximumMappedStaticFiles 映射缓存的条目上限，取自登记那一刻的连接级限额
         * @note 重复调用是空操作：路由只登记一次，之后再改目录/开关只写配置对象
         */
        void install(Router &router, std::size_t maximumMappedStaticFiles);

        /**
         * @brief 设置（或关闭）静态文件根目录
         * @param directoryPath UTF-8 文本，相对或绝对均可；空串表示关闭
         * @details 目录在此刻被规范化：不存在或无权访问即关闭静态服务并记一条中文告警，
         *          不把这个 surprise 留到第一个请求上。规范化结果会被缓存，因此运行期间把该目录
         *          改名或移动，行为不再可预期。
         */
        void setDirectory(const std::string &directoryPath);

        /**
         * @brief 读回当前生效的静态根目录
         * @return std::string 规范化后的绝对路径（UTF-8 文本，与配置时同一刻度）；未启用时为空串
         */
        [[nodiscard]] std::string directory() const;

        /**
         * @brief 设置静态文件响应的 Cache-Control 头值
         * @param cacheControl 值原样写进头部块；含 CR/LF/NUL 时忽略这条配置并记告警（响应拆分的门）；
         *        空 optional 表示不发这条头
         */
        void setCacheControl(std::optional<std::string> cacheControl);

        /**
         * @brief 设置请求打到目录本身时要不要生成 HTML 目录列表
         * @param enabled true 列出该目录的条目；false（默认）按 404 处理，与关闭静态目录时的形状一致
         * @details 列表是现读生成的，因此不带验证器且带 `cache-control: no-store`：一份被缓存
         *          下来的列表会在目录内容变化之后继续对外宣称「这里有这些文件」。条目数有上界
         *          （超出时在末尾如实写明还有多少项没列出）。Range 对列表一律忽略——它不是一个
         *          稳定的表示，给它切片等于承诺一个不存在的长度。
         * @note 只在显式打开时才多问一次「这是目录吗」：关着的时候探测者拿到的仍是一条普通 404，
         *       不泄露目录结构
         * @see StaticFileSettings::listingEnabled
         */
        void setDirectoryListing(bool enabled);

        /**
         * @brief 设置静态服务要不要按 `Accept-Encoding` 挑选同目录的预压缩副本
         * @param enabled true 时按服务器偏好挑「对端接受且磁盘上有副本」的那份；false（默认）只发请求路径本身
         * @details 打开后同一 URL 存在两个表示，于是 `Vary: Accept-Encoding` 无条件跟着发——哪怕这一条
         *          请求最终发的是明文：缓存见过 br 那份再把它发给不接受 br 的客户端就是投毒。
         *          验证器随表示变：ETag 由实际发出去那份文件的大小与整秒修改时间构出，明文与副本
         *          天然不同，跨变体的 `If-None-Match` 因此不会互相命中。
         * @note 副本由部署方自己生成（引擎不压盘上的文件），算法名与后缀的对应见 `Detail::kCompressionCodecs`
         * @note 请求路径本身不存在时不会退回去发某个副本，仍按 404：变体不能凭空造出一个资源
         * @see StaticFileSettings::precompressedVariantsEnabled
         */
        void setPrecompressedVariants(bool enabled);

        /**
         * @brief 设置（或关闭）RFC 9842 的压缩字典：静态资源按 `Available-Dictionary` 协商发 `.dcz` 副本
         * @param relativePath 字典文件相对静态根的路径（如 `"assets/app.dict"`）；空串表示关闭
         * @param matchPattern `Use-As-Dictionary` 的 match 取值（URL 路径模式，如 `"/assets/*"`）；
         *        空串一并表示关闭
         * @details 配置期做四件事：把相对路径解析到根内并确认是常规文件、读全文算 SHA-256、校验模式
         *          （非空、以 `/` 开头、无控制字符——sf-string 里它们非法）、把三件事实冻进
         *          `StaticFileSettings::dictionary`。任何一件不成立就不启用并记一条中文告警，
         *          与 setDirectory 同一条口径：把 surprise 留在配置期，不留给第一个请求。
         *
         *          生效面（RFC 9842 §8 要求安全上下文，三条缺一不可）：
         *          @li 连接是 TLS（明文 HTTP 既不发 `Use-As-Dictionary` 也不发 `dcz`）；
         *          @li 变体协商开着（`.dcz` 是副本形态之一，靠 setPrecompressedVariants 这一格）；
         *          @li 请求带 `Available-Dictionary` 且哈希与本字典逐字节相等。
         *          三条成立且磁盘上有 `.dcz` 副本时它优先于其余副本；任一条不成立就退回既有协商，
         *          响应与没配这本字典时逐字相同。
         * @note `.dcz` 副本由部署方用同一本字典离线压出（`zstd -D assets/app.dict app.js` 之类），
         *       引擎不压盘上的文件——与其余预压缩副本同一条分工
         * @note 一本表只支持一本字典：多本（各带自己的 match 域）等真有部署这么用时再谈，届时按
         *       §2.2.3 的「最长 match 优先」挑
         * @see StaticFileCompressionDictionary
         */
        void setCompressionDictionary(const std::string &relativePath, const std::string &matchPattern);

        /**
         * @brief 取配置本体（兜底路由的处理函数持有的就是它）
         * @return std::shared_ptr<StaticFileSettings> 未 install() 时为空指针
         */
        [[nodiscard]] std::shared_ptr<StaticFileSettings> settings() const noexcept;

    private:
        std::shared_ptr<StaticFileSettings> m_settings; ///< 配置本体；空表示还没登记过兜底路由
    };

} // namespace AsynGyanis::Net
