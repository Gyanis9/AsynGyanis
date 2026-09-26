/**
 * @file StaticFileService.h
 * @brief 静态文件目录这项配置的本体与它的兜底路由登记：明文 HTTP、HTTPS 与 HTTP/3 三条服务器共用
 * @author Gyanis
 * @date 2026-09-26
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

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
     * @brief 静态文件服务的运行时配置：开关、规范化后的根目录、Cache-Control 与映射缓存
     *
     * @details 由兜底路由的处理函数按 shared_ptr 只读持有，服务器一侧改的就是这个对象里的字段。
     *          按值捕获一份共享指针比捕获 `this` 更安全——处理函数可能在服务器之后才被销毁。
     * @note 只在事件循环线程（或启动前的配置期）读写：静态服务开关与目录的变更不会与在途请求交错。
     */
    struct StaticFileSettings
    {
        bool                       isEnabled{false}; ///< 是否启用静态文件服务；根目录规范化失败即为 false
        std::filesystem::path      rootDirectory;    ///< 规范化（weakly_canonical）之后的静态根目录，绝对路径
        std::optional<std::string> cacheControl;     ///< 静态文件响应的 Cache-Control 值；空表示不发这条头
        /**
         * @brief 映射缓存，由 StaticFileService::install() 按当时的限额建立，之后只读
         * @note 条目上限取自登记那一刻的 HttpServerLimits::maximumMappedStaticFiles，
         *       因此要改上限必须先 setLimits() 再设静态目录
         */
        std::shared_ptr<StaticFileMappingCache> mappingCache;
    };

    /**
     * @brief 「静态文件目录」配置的本体：把配置动作与那条 `any("*")` 兜底路由的登记收在一处
     *
     * @details 存在的理由：静态服务原先只长在 HttpServer 上，配置动作、目录规范化、Cache-Control 的
     *          合法性判定与兜底路由的登记全写死在它的方法里。HTTPS 与 h3 要接同一能力，只有两条路——
     *          把那一整段抄第二份、第三份（漂移的表现是「明文侧修了一个路径穿越，TLS 侧没有」这种
     *          最贵的一类），或者把本体收出来。这里收的是本体，各服务器一侧只留转发，语义逐字相同。
     * @note 成员函数的**定义在 HttpServer.cpp**：处理函数 serveStaticFileRequest 与它要用的路径清洗、
     *       条件请求判定都在那，搬来搬去只会把一处实现拆成两处读。
     * @warning install() 之前不能调其余配置方法（那是各服务器的 ensure 转发负责的事）。
     * @see StaticFileSettings, HttpServer::staticFileDir()
     */
    class StaticFileService
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
         * @details 目录在**此刻**被规范化：不存在或无权访问即关闭静态服务并记一条中文告警，
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
         * @brief 取配置本体（兜底路由的处理函数持有的就是它）
         * @return std::shared_ptr<StaticFileSettings> 未 install() 时为空指针
         */
        [[nodiscard]] std::shared_ptr<StaticFileSettings> settings() const noexcept;

    private:
        std::shared_ptr<StaticFileSettings> m_settings; ///< 配置本体；空表示还没登记过兜底路由
    };

} // namespace AsynGyanis::Net
