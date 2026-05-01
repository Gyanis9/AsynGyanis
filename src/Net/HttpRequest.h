/**
 * @file HttpRequest.h
 * @brief 解析后的 HTTP 请求数据对象
 * @copyright Copyright (c) 2026
 */
#ifndef NET_HTTPREQUEST_H
#define NET_HTTPREQUEST_H

#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Net
{
    /**
     * @brief HTTP 方法枚举。
     */
    enum class HttpMethod
    {
        GET,     ///< GET 方法
        POST,    ///< POST 方法
        PUT,     ///< PUT 方法
        DELETE,  ///< DELETE 方法
        PATCH,   ///< PATCH 方法
        HEAD,    ///< HEAD 方法
        OPTIONS, ///< OPTIONS 方法
        UNKNOWN  ///< 未知方法
    };

    /**
     * @brief HTTP 请求数据对象。
     *
     * 存储解析后的 HTTP 请求内容，包括方法、URI、版本、头部、正文、路由参数等。
     * 支持查询头部、解析查询参数、获取路径等操作。
     */
    class HttpRequest
    {
    public:
        /**
         * @brief 默认构造函数，创建一个空请求对象。
         */
        HttpRequest() = default;

        /**
         * @brief 将字符串转换为 HttpMethod 枚举。
         * @param method HTTP 方法字符串，如 "GET"、"POST"
         * @return 对应的 HttpMethod 枚举值（若未知则返回 HttpMethod::UNKNOWN）
         */
        static HttpMethod methodFromString(std::string_view method);

        /**
         * @brief 设置 HTTP 请求方法。
         * @param method HTTP 方法枚举值
         */
        void setMethod(HttpMethod method);

        /**
         * @brief 获取 HTTP 请求方法。
         * @return HttpMethod 枚举值
         */
        HttpMethod method() const;

        /**
         * @brief 设置请求 URI（原始请求行中的 URI）。
         * @param uri URI 字符串
         */
        void setUri(std::string uri);

        /**
         * @brief 获取原始 URI。
         * @return URI 字符串引用
         */
        const std::string &uri() const;

        /**
         * @brief 设置 HTTP 版本（如 "HTTP/1.1"）。
         * @param version 版本字符串
         */
        void setHttpVersion(std::string version);

        /**
         * @brief 获取 HTTP 版本。
         * @return 版本字符串引用
         */
        const std::string &httpVersion() const;

        /**
         * @brief 添加一个 HTTP 头部字段。
         * @param key   头部字段名
         * @param value 头部字段值
         */
        void addHeader(std::string key, std::string value);

        /**
         * @brief 获取指定名称的 HTTP 头部值。
         * @param key 头部字段名（不区分大小写，但内部存储为原始大小写）
         * @return 若存在则返回头部值的 optional，否则为空
         */
        std::optional<std::string> getHeader(const std::string &key) const;

        /**
         * @brief 获取所有头部字段的只读引用。
         * @return 头部 unordered_map 引用
         */
        const std::unordered_map<std::string, std::string> &headers() const;

        /**
         * @brief 设置请求正文。
         * @param body 正文内容
         */
        void setBody(std::string body);
        void appendBody(const char *data, size_t len);

        /**
         * @brief 获取请求正文（字符串视图）。
         * @return 正文内容的 string_view
         */
        std::string_view body() const;

        /**
         * @brief 从 URI 中提取路径部分（不含查询参数）。
         * @return 路径字符串
         */
        std::string path() const;

        /**
         * @brief 解析 URI 中的查询参数（key=value 对）。
         * @return 查询参数的 unordered_map
         */
        std::unordered_map<std::string, std::string> queryParams() const;

        /**
         * @brief 设置路由参数（如路径参数）。
         * @param key   参数名
         * @param value 参数值
         */
        void setParam(std::string key, std::string value);

        /**
         * @brief 获取路由参数值。
         * @param key 参数名
         * @return 参数值的 optional，若不存在则为空
         */
        std::optional<std::string> param(const std::string &key) const;

        /**
         * @brief 重置请求对象，清空所有字段（方法、URI、头部、正文、参数等）。
         */
        void reset();

        // ====================================================================
        // 协作式取消支持（用于超时中间件等场景）
        // ====================================================================

        /**
         * @brief 获取与本次请求关联的取消令牌。
         *
         * 中间件（如 timeoutMiddleware）可设置 stop_source，业务处理器可定期检查
         * token.stop_requested() 以实现协作式取消，避免超时后继续浪费资源。
         */
        [[nodiscard]] std::stop_token cancelToken() const noexcept;

        /**
         * @brief 获取内部 stop_source 的可写引用，供中间件设置取消信号。
         */
        [[nodiscard]] std::stop_source &cancelSource() noexcept;

        /**
         * @brief 请求取消本次处理（由中间件在超时等条件下调用）。
         * @return 是否成功发出取消请求
         */
        bool requestCancel() const;

    private:
        HttpMethod                                   m_method{HttpMethod::UNKNOWN}; ///< HTTP 方法
        std::string                                  m_uri;                         ///< 原始 URI
        std::string                                  m_httpVersion;                 ///< HTTP 版本
        std::unordered_map<std::string, std::string> m_headers;                     ///< 头部字段（键值对）
        std::string                                  m_body;                        ///< 消息正文
        std::unordered_map<std::string, std::string> m_params;                      ///< 路由参数
        std::stop_source                             m_cancelSource;                ///< 协作式取消源
    };
}

#endif
