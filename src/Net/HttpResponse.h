/**
 * @file HttpResponse.h
 * @brief HTTP 响应构建器与序列化器
 * @copyright Copyright (c) 2026
 */
#ifndef NET_HTTPRESPONSE_H
#define NET_HTTPRESPONSE_H

#include <string>
#include <string_view>
#include <unordered_map>

namespace Net
{
    /**
     * @brief HTTP 响应类，用于构建和序列化 HTTP 响应消息。
     *
     * 支持设置状态码、头部、正文，并提供 toString() 方法生成符合 HTTP/1.1 规范的响应字符串。
     * 包含常用的静态工厂方法，如 ok()、notFound()、serverError()。
     */
    class HttpResponse
    {
    public:
        /**
         * @brief 构造一个默认响应（状态码 200 OK，无正文）。
         */
        HttpResponse();

        /**
         * @brief 设置 HTTP 状态码。
         * @param code 状态码，如 200、404、500
         */
        void setStatus(int code);

        /**
         * @brief 获取当前状态码。
         * @return 状态码
         */
        int status() const;

        /**
         * @brief 设置一个 HTTP 头部字段。
         * @param name  头部字段名（如 "Content-Type"）
         * @param value 头部字段值（如 "text/html"）
         */
        void setHeader(const std::string &name, const std::string &value);

        /**
         * @brief 获取所有头部字段的只读引用。
         * @return 头部 unordered_map
         */
        const std::unordered_map<std::string, std::string> &headers() const;

        /**
         * @brief 设置响应正文。
         * @param body 正文字符串视图（内容将被复制存储）
         */
        void setBody(std::string_view body);

        /**
         * @brief 获取响应正文。
         * @return 正文字符串视图
         */
        std::string_view body() const;

        /**
         * @brief 设置 HTTP 协议版本（默认 "HTTP/1.1"），用于状态行序列化。
         */
        void setHttpVersion(std::string version);

        /**
         * @brief 将响应序列化为 HTTP 格式的字符串。
         * @return 完整的 HTTP 响应字符串（包含状态行、头部、空行、正文）
         */
        std::string toString() const;

        /**
         * @brief 创建一个 200 OK 响应。
         * @param body 响应正文
         * @return HttpResponse 对象
         */
        static HttpResponse ok(std::string body);

        /**
         * @brief 创建一个 404 Not Found 响应。
         * @return HttpResponse 对象
         */
        static HttpResponse notFound();

        /**
         * @brief 创建一个 500 Internal Server Error 响应。
         * @param msg 错误描述正文（可选，默认为空）
         * @return HttpResponse 对象
         */
        static HttpResponse serverError(std::string msg = "");

        /**
         * @brief 重置响应对象到初始状态（状态码 200，清空头部和正文）。
         */
        void reset();

    private:
        /**
         * @brief 根据状态码获取标准原因短语。
         * @param code 状态码
         * @return 原因短语字符串（如 "OK"、"Not Found"）
         */
        static const char *statusMessage(int code);

        int                                          m_status;                  ///< HTTP 状态码
        std::string                                  m_httpVersion{"HTTP/1.1"}; ///< HTTP 版本，默认 1.1
        std::unordered_map<std::string, std::string> m_headers;                 ///< 头部字段映射
        std::string                                  m_body;                    ///< 响应正文
    };
}

#endif
