/**
 * @file HttpParser.h
 * @brief HTTP/1.1 解析器，封装 llhttp C 库
 * @copyright Copyright (c) 2026
 */
#ifndef NET_HTTPPARSER_H
#define NET_HTTPPARSER_H

#include "HttpRequest.h"

#include <llhttp.h>

#include <string>

namespace Net
{
    /**
     * @brief 解析状态枚举。
     */
    enum class ParseStatus
    {
        Ok,       ///< 正常解析，可能未完成
        Error,    ///< 解析错误
        NeedMore, ///< 数据不足，需要更多输入
        Done      ///< 消息解析完成
    };

    /**
     * @brief HTTP/1.1 解析器，基于 llhttp 库。
     *
     * 用于增量解析 HTTP 请求或响应。支持 URL、头部、正文等字段提取。
     * 使用回调机制将解析结果填充到 HttpRequest 对象中。
     */
    class HttpParser
    {
    public:
        /**
         * @brief 构造 HttpParser 对象，初始化 llhttp 解析器及其设置。
         */
        HttpParser();

        /**
         * @brief 析构函数
         */
        ~HttpParser();

        HttpParser(const HttpParser &)            = delete;
        HttpParser &operator=(const HttpParser &) = delete;

        /**
         * @brief 解析输入数据块。
         * @param data 数据缓冲区指针
         * @param len  数据长度
         * @return 解析状态，可指示成功、错误、需要更多数据或完成
         */
        ParseStatus parse(const char *data, size_t len);

        /**
         * @brief 重置解析器状态，以便重新解析新消息。
         */
        void reset();

        /**
         * @brief 获取当前解析出的 HTTP 请求对象引用。
         * @return HttpRequest&
         */
        HttpRequest &request();

        /**
         * @brief 检查解析器是否处于错误状态。
         * @return true 表示发生错误，false 表示无错误
         */
        bool hasError() const;

        /**
         * @brief 获取错误信息描述（若有）。
         * @return 错误字符串
         */
        std::string errorMessage() const;

    private:
        // 静态回调函数，由 llhttp 调用，用于通知解析器不同部分
        static int onUrl(llhttp_t *parser, const char *data, size_t len);         ///< URL 回调
        static int onHeaderField(llhttp_t *parser, const char *data, size_t len); ///< 头部字段名回调
        static int onHeaderValue(llhttp_t *parser, const char *data, size_t len); ///< 头部字段值回调
        static int onBody(llhttp_t *parser, const char *data, size_t len);        ///< 消息体回调
        static int onMessageComplete(llhttp_t *parser);                           ///< 消息完成回调

        llhttp_t          m_parser{};           ///< llhttp 解析器实例
        llhttp_settings_t m_settings{};         ///< llhttp 回调设置
        HttpRequest       m_currentRequest;     ///< 当前正在构建的 HTTP 请求
        std::string       m_currentHeaderField; ///< 当前正在解析的头部字段名
        std::string       m_currentUrl;         ///< 当前正在解析的 URL
        bool              m_error{false};       ///< 是否发生解析错误
        std::string       m_errorMessage;       ///< 错误描述消息
        bool              m_complete{false};    ///< 是否已完成整个消息解析
    };

}

#endif
