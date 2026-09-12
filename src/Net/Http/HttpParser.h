/**
 * @file HttpParser.h
 * @brief HTTP/1.1 请求报文增量解析器（手写状态机，无外部依赖）
 * @author Gyanis
 * @date 2026-09-12
 * @version 2.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Net/Http/HttpParseErrorKind.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/ParseStatus.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief HTTP/1.1 请求报文解析器：手写增量状态机
     *
     * @details 以增量方式解析请求：每次把新读到的字节交给 parse()，按「请求行 → 头部块 → 正文」
     *          三段推进，解析结果先落在内部的暂存请求对象上，**只有一条报文收齐的那一刻**
     *          才整体搬进对外可见的 request()。于是半成品阶段 request() 一定是空壳——
     *          上层连「过程数据」都无从误用，比「可读但不许放行」更强。
     *
     * ## 状态推进
     * @li 请求行：`方法 SP 目标 SP HTTP/主.次`，三部分都按 token 规则严格校验；
     * @li 头部块：每行 `名: 值`，名必须是 token（冒号前不得有空白），值的首尾 OWS 裁掉、
     *     其余原样保留；空行表示头部块结束；
     * @li 正文：按 Content-Length 收满即完成。没有 Content-Length 的报文在头部块结束时即完成，
     *     多余的字节属于下一条报文，一个都不吃。
     *
     * 任何一步都可以在**任意字节边界**被切开：半行、半个 CRLF、正文中间都能续上。
     * 已完成（或已失败）之后再喂数据不会改变既有结果：前者一字节不吃，后者维持粘滞错误。
     *
     * ## 与上层的关系
     * 本解析器不认识「一条报文到哪里结束」之外的任何策略：分块请求体、头部过大这些判断
     * 由 HttpSession 的定界器先行处理并回 431/413/411，能喂到这里的请求已经过它筛选。
     * 解析器自己仍保留一份独立的资源上限（见下），两侧互不依赖——直接把解析器当库用的
     * 调用方同样受保护。
     *
     * @warning 所有资源上限都是 DoS 防护：任何一项超限都会以 Error 结束本次解析，
     *          并用 isLimitExceeded() 标出「超限」这一子类，便于上层回 431/413 而不是 400。
     *          上限是策略而非协议要求，取值依据见各常量的行尾注释。
     */
    class HttpParser
    {
    public:
        /**
         * @brief 构造解析器：全部状态为默认值，可直接开始解析。
         */
        HttpParser() = default;

        /**
         * @brief 析构函数：所有成员都是按值的标准容器，无额外资源需要回收。
         */
        ~HttpParser() = default;

        // 禁拷贝也禁移动：解析器内部持有请求对象与暂存缓冲，复制一份没有任何使用场景，
        // 而按值传递的错误用法会静默地把「解析到一半的状态」分叉成两份
        HttpParser(const HttpParser &)            = delete;
        HttpParser &operator=(const HttpParser &) = delete;

        /**
         * @brief 解析一段输入数据。
         *
         * @details 状态判定可推理，三步互斥且穷尽：
         *          @li 先看是否已收齐：收齐就返回 Done，且不再消耗任何字节
         *              （多余字节属于流水线里的下一条报文，喂进已完成的解析器会破坏上一条结果）；
         *          @li 再看解析过程中是否失败：失败则返回 Error 且错误粘滞；
         *          @li 否则返回 NeedMore，表示本段字节已全部消费但报文还没收齐。
         *
         * @param data   数据起始指针，调用方保证可读
         * @param length 数据长度，单位字节
         * @retval ParseStatus::Done 一条完整报文收齐，request() 的全部内容此刻才可读
         * @retval ParseStatus::NeedMore 仍需更多数据；此时 request() 一定是空壳（尚未搬运）
         * @retval ParseStatus::Error 解析失败，request() 只能视为不可信并丢弃；
         *                            解析器进入粘滞错误态，除非 reset()，后续调用仍返回 Error
         * @see ParseStatus, isLimitExceeded(), errorMessage()
         */
        ParseStatus parse(const char *data, size_t length);

        /**
         * @brief 重置解析器状态，以便解析下一条消息。
         * @details 清空请求对象、暂存请求与全部半成品缓冲、错误标记，回到「等待请求行」的初态。
         */
        void reset();

        /**
         * @brief 获取当前解析出的 HTTP 请求对象引用。
         * @details 仅在 parse() 返回 Done 后内容完整可用；NeedMore 阶段是空壳。
         * @return HttpRequest& 引用，生命周期跟随本解析器
         */
        HttpRequest &request();

        /**
         * @brief 取回最近一次 parse() 调用实际消费的字节数
         *
         * @details 这是「报文到哪里结束」的唯一出处：Done 之后，喂进去的这段数据里前
         *          consumedByteCount() 个字节属于本条报文，排在后面的字节属于流水线里的
         *          下一条报文（本次调用一个都没吃，留给调用方自己留好）。调用方据此把缓冲区
         *          的已消费前缀挪掉，下一轮接着解析剩下的。
         * @return std::size_t 已消费字节数；调用失败或已完成之后再喂数据时为 0
         * @see parse()
         */
        [[nodiscard]] std::size_t consumedByteCount() const;

        /**
         * @brief 检查解析器是否处于错误状态。
         * @return true 表示发生过错误（含超出资源上限），false 表示无错误
         */
        [[nodiscard]] bool hasError() const;

        /**
         * @brief 检查本次错误是否由资源上限触发。
         * @details 与 hasError() 联合使用即可从 Error 里再分出「超限」这一子类：
         *          超限说明对端报文形态合法但体量越界，上层宜回 431（头部过大）或 413（正文过大），
         *          而不是笼统的 400。
         * @return true 表示错误由本解析器的某项资源上限造成
         */
        [[nodiscard]] bool isLimitExceeded() const;

        /**
         * @brief 获取错误信息描述（若有）。
         * @return 面向使用者的中文错误文本；无错误时为空串
         */
        [[nodiscard]] std::string errorMessage() const;

        /**
         * @brief 获取本次失败的类别
         * @details 上层据它决定回哪个状态码（400/431/413/411），不必去匹配错误文案
         * @return HttpParseErrorKind 失败类别；未失败时为 None
         */
        [[nodiscard]] HttpParseErrorKind errorKind() const;

    private:
        /**
         * @brief 解析所处阶段
         */
        enum class Stage
        {
            RequestLine, ///< 正在收请求行
            Headers,     ///< 正在收头部行
            Body,        ///< 正在收正文
            Complete,    ///< 本条已收齐：再喂数据一字节不吃
            Failed       ///< 已失败：错误粘滞到 reset()
        };

        /**
         * @brief 取出一整行（以 CRLF 结尾）
         * @details 交回的视图要么指向本次输入（行完整落在这一段里，零拷贝），
         *          要么指向内部的半行暂存（行被切在两次调用之间）。视图在下一次
         *          takeLine() 调用前有效——调用方必须当场解析完。
         * @param data 输入起始指针
         * @param length 输入长度
         * @param consumed [in,out] 已消费字节数，取到整行时推进到该行之后
         * @param line 输出参数：去掉 CRLF 的行内容
         * @return true 取到一整行；false 表示本段输入凑不齐一行（剩余字节已并入半行暂存），
         *         或本行的长度已越过该阶段的上限（此时已记录超限错误）
         */
        [[nodiscard]] bool takeLine(const char *data, std::size_t length, std::size_t &consumed, std::string_view &line);

        /**
         * @brief 按当前阶段校验一行（含尚未收尾的半行）的长度上限
         * @details 兜住「一行永远不结束」的输入：没有它，一个超长的请求行或头部行
         *          就能让解析器的暂存一直膨胀下去。
         * @param length 待校验的行长度（半行含已暂存部分）
         * @return true 未超限；false 已记录超限错误
         */
        [[nodiscard]] bool checkLineLength(std::size_t length);

        /**
         * @brief 解析请求行并填充暂存请求的方法与版本
         * @param line 去掉 CRLF 的请求行
         * @return true 合法
         */
        [[nodiscard]] bool parseRequestLine(std::string_view line);

        /**
         * @brief 解析一条头部行
         * @param line 去掉 CRLF 的头部行；空行表示头部块结束
         * @return true 合法
         */
        [[nodiscard]] bool parseHeaderLine(std::string_view line);

        /**
         * @brief 解析 Content-Length 的值，并处理重复出现
         * @param value 已裁掉首尾 OWS 的值
         * @return true 合法（重复但取值一致也判合法，与上层定界器的口径一致）
         */
        [[nodiscard]] bool parseContentLength(std::string_view value);

        /**
         * @brief 记录一次协议级非法（Malformed，上层回 400）
         * @param message 中文错误详情
         */
        void failMalformed(std::string message);

        /**
         * @brief 记录一次头部超限（HeaderTooLarge，上层回 431）
         * @param message 中文错误详情，须含具体上限数值
         */
        void failHeaderTooLarge(std::string message);

        /**
         * @brief 记录一次正文超限（BodyTooLarge，上层回 413）
         * @param message 中文错误详情，须含具体上限数值
         */
        void failBodyTooLarge(std::string message);

        /**
         * @brief 记录一次「分块请求体不支持」（ChunkedNotSupported，上层回 411）
         * @param message 中文错误详情
         */
        void failChunkedNotSupported(std::string message);

        /**
         * @brief 统一的失败记录：置粘滞错误态，并记下类别与中文详情
         * @param errorKind 失败类别
         * @param message 中文错误详情
         */
        void recordFailure(HttpParseErrorKind errorKind, std::string message);

        /**
         * @brief 头部块结束：按有无 Content-Length 决定直接完成还是转入正文阶段
         */
        void finishHeaderBlock();

        /**
         * @brief 移交：把暂存的解析结果交给对外请求对象，并把暂存清回可复用的初态
         */
        void commitMessage();

        /**
         * @brief 清空单条报文的暂存（容器只清内容、保留容量，供下一条报文复用）
         */
        void clearMessageScratch() noexcept;

        /**
         * @brief 解析中的一条头部（名与值都按收到的原文暂存，提交时才移交出去）
         */
        struct ParsedHeader
        {
            std::string name;  ///< 头部名原文（大小写保持，交给请求对象时归一化）
            std::string value; ///< 头部值原文（首尾 OWS 已裁掉）
        };

        Stage m_stage{Stage::RequestLine}; ///< 当前阶段

        HttpRequest m_currentRequest; ///< 对外可见的请求对象，只在 Done 那一刻被填充

        // 解析过程的暂存：全部在解析器内部，收齐那一刻才整体移交。容器跨报文复用
        //（clear 保留容量），因此稳态下不产生额外分配
        HttpMethod                m_method{HttpMethod::UNKNOWN}; ///< 请求方法
        std::string               m_uri;                         ///< 请求目标
        std::string               m_httpVersion;                 ///< 版本原文
        std::vector<ParsedHeader> m_headers;                     ///< 已解析的头部，按到达顺序
        std::string               m_body;                        ///< 已收正文

        std::string m_pendingLine; ///< 尚未等到 CRLF 的半行（可能跨多次 parse()）

        /// 半行暂存是否已作为视图交出去（交出去的视图用完之前不能清，收回之前不能拼）
        bool m_isPendingLineHandedOut{false};

        std::size_t m_contentLength{0};        ///< 本条报文的正文长度（Content-Length，缺省 0）
        std::size_t m_receivedBodyLength{0};   ///< 已收正文字节数
        std::size_t m_headerFieldCount{0};     ///< 已解析头部条数
        std::size_t m_headerBlockLength{0};    ///< 头部块净字节数，只算名与值，不含 ": " 与 CRLF
        bool        m_hasContentLength{false}; ///< 是否已见过 Content-Length（用于比对重复值）

        bool        m_hasError{false};        ///< 是否已发生解析错误
        HttpParseErrorKind m_errorKind{HttpParseErrorKind::None}; ///< 失败类别（决定上层回哪个状态码）
        std::string m_errorMessage;           ///< 面向使用者的中文错误描述
        std::size_t m_consumedByteCount{0};   ///< 最近一次 parse() 实际消费的字节数

        // 资源上限：全部按「正常流量远达不到、恶意流量立刻撞线」的口径取值，单位统一为字节。
        // 任何一项超限都走 Error + isLimitExceeded()，绝不静默截断后继续解析
        static constexpr std::size_t kMaximumBodySize = 8ull * 1024 * 1024;        ///< 请求体上限 8 MiB：够上传小文件，不够拖垮内存，与限流中间件的默认档位一致
        static constexpr std::size_t kMaximumUriLength = 8ull * 1024;              ///< 请求 URI 上限 8 KiB：对齐 nginx large_client_header_buffers 的单行 8 KiB，浏览器实际 URI 远低于此
        static constexpr std::size_t kMaximumHeaderFieldNameLength = 256;          ///< 单个头部名上限 256 B：标准头部名最长不过数十 B，留足私有前缀（x-amz- 等）后仍宽裕
        static constexpr std::size_t kMaximumHeaderFieldValueLength = 8ull * 1024; ///< 单个头部值上限 8 KiB：与 URI 同档，覆盖超长 Cookie 头的现实用量
        static constexpr std::size_t kMaximumHeaderCount = 100;                    ///< 头部条数上限 100 条：浏览器实际请求不足 40 条，此值专防「海量空值头部」撑爆容器节点
        static constexpr std::size_t kMaximumHeaderBlockLength = 64ull * 1024;     ///< 头部块总长上限 64 KiB：名与值净字节之和，条数与单条之外的第三道闸，约合 8 个 8 KiB 接收缓冲区

        /// 方法原文上限 32 B：llhttp 同档取值，通用方法最长 7 B（OPTIONS），留足自定义动词余地
        static constexpr std::size_t kMaximumMethodLength = 32;

        /// 请求行上限 = URI 上限 + 方法上限 + "HTTP/9.9" 与两个分隔空格，用于兜住「一行始终不结束」的输入
        static constexpr std::size_t kMaximumRequestLineLength = kMaximumUriLength + kMaximumMethodLength + 16;

        /// 头部行上限 = 名上限 + 值上限 + ": " 与 CRLF，同样用于兜住始终不结束的一行
        static constexpr std::size_t kMaximumHeaderLineLength = kMaximumHeaderFieldNameLength + kMaximumHeaderFieldValueLength + 4;
    };
} // namespace AsynGyanis::Net
