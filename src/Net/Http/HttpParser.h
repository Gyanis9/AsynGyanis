/**
 * @file HttpParser.h
 * @brief HTTP/1.1 请求报文增量解析器（手写状态机，无外部依赖）
 * @author Gyanis
 * @date 2026-09-13
 * @version 2.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Net/Http/HttpParseErrorKind.h"
#include "Net/Http/HttpParserLimits.h"
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
     * @details 按「请求行 → 头部块 → 正文」三段推进，请求行与头部行都按 token 规则严格校验。正文由
     *          Content-Length 或 Transfer-Encoding: chunked 定界：分块按 RFC 9112 §7.1 增量解码，
     *          trailer 段只校验语法与上限、不并入请求头部；两者都没有的报文在头部块结束时即完成。
     *          任何字节边界都能切开续上，解析结果只在报文收齐那一刻整体搬进 request()。
     *
     * @warning 所有资源上限都是 DoS 防护：任何一项超限都会以 Error 结束本次解析，并用
     *          isLimitExceeded() 标出「超限」这一子类，便于上层回 431/413 而不是 400；
     *          上限是策略而非协议要求，默认取值与「0 表示关闭该项保护」的语义见 HttpParserLimits，
     *          本类在构造时按值固定一份，运行期不可更换。
     * @warning 定界头的组合从严：Transfer-Encoding 与 Content-Length 并存、取值不是唯一的 chunked
     *          一律判错（RFC 9112 §6.3），不接受 gzip 等其它传输编码，也不悄悄按 identity 处理。
     */
    class HttpParser
    {
    public:
        /**
         * @brief 构造解析器：取一份资源上限配置，其余状态为默认值，可直接开始解析。
         * @param limits 资源上限配置；默认值即 HttpParserLimits 的缺省字段。配置按值拷入，构造之后
         *        本解析器一直用它，没有中途更换的入口（见 HttpParserLimits 的 @note）
         */
        explicit HttpParser(HttpParserLimits limits = {});

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
         * @details 上层据它决定回哪个状态码（400/431/413），不必去匹配错误文案
         * @return HttpParseErrorKind 失败类别；未失败时为 None
         */
        [[nodiscard]] HttpParseErrorKind errorKind() const;

    private:
        /**
         * @brief 解析所处阶段
         */
        enum class Stage
        {
            RequestLine,         ///< 正在收请求行
            Headers,             ///< 正在收头部行
            Body,                ///< 正在收 Content-Length 定界的正文
            Complete,            ///< 本条已收齐：再喂数据一字节不吃
            Failed,              ///< 已失败：错误粘滞到 reset()
            ChunkSize,           ///< 正在收分块块大小行（含块扩展），以 CRLF 结尾
            ChunkData,           ///< 正在收分块块数据
            ChunkDataTerminator, ///< 正在收块数据之后的 CRLF
            Trailer              ///< 正在收 trailer 段，空行表示整条报文收齐
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
         * @brief 按「名: 值」语法校验一行字段并切出名字与值
         * @details 头部行与 trailer 行共用这一份校验：折行、冒号位置、名与值的字符集、
         *          单条长度以及累计条数与总长上限都在这里判，两者不各写一份而漂移。
         * @param line 去掉 CRLF 的字段行，调用方保证非空
         * @param fieldContextLabel 出错文案里的字段归属（如「请求头部」「trailer 头部」）
         * @param name 输出参数：字段名原文，返回 true 时有效
         * @param value 输出参数：已裁掉首尾 OWS 的字段值，返回 true 时有效
         * @return true 合法
         */
        [[nodiscard]] bool parseFieldLine(std::string_view line, std::string_view fieldContextLabel,
                                          std::string_view &name, std::string_view &value);

        /**
         * @brief 按当前阶段校验一行（含尚未收尾的半行）的长度上限
         * @details 兜住「一行永远不结束」的输入：没有它，一个超长的请求行或头部行
         *          就能让解析器的暂存一直膨胀下去。对应上限为 0 表示该项保护已关闭，本函数直接放行。
         * @param length 待校验的行长度（半行含已暂存部分）
         * @return true 未超限；false 已记录超限错误
         */
        [[nodiscard]] bool checkLineLength(std::size_t length);

        /**
         * @brief 推导「一行头部」整行的长度上限
         * @details 名与值两项各有一道上限，整行还得有第三道闸才拦得住「名与值都合规但拼起来超长」的行。
         * @return std::size_t 整行上限（名上限 + 值上限 + ": " 与 CRLF）；0 表示不设上限
         */
        [[nodiscard]] std::size_t headerLineLengthLimit() const noexcept;

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
         * @brief 累积一条 Transfer-Encoding 取值，供头部块结束时统一裁决
         * @param value 已裁掉首尾 OWS 的取值
         * @return true 合法（取值非空）
         */
        [[nodiscard]] bool appendTransferEncodingValue(std::string_view value);

        /**
         * @brief 解析分块块大小行：十六进制大小加可选的块扩展
         * @details 按 RFC 9112 §7.1.1 校验：大小至少一位十六进制数字、必须带 CRLF（由取行阶段保证），
         *          扩展只校验语法不解释语义；解析出的零是终止块，随后的字节进入 trailer 段。
         * @param line 去掉 CRLF 的块大小行
         * @return true 合法
         */
        [[nodiscard]] bool parseChunkSizeLine(std::string_view line);

        /**
         * @brief 校验一条 trailer 行并按需收尾整条报文
         * @details 语法与上限同头部行；内容有意不并入请求头部，避免 trailer 里的 content-length
         *          之类与已解析头部形成两种解释（请求走私面），RFC 9112 §7.1.2 允许收端忽略 trailer。
         * @param line 去掉 CRLF 的 trailer 行；空行表示 trailer 段结束、报文收齐
         * @return true 合法
         */
        [[nodiscard]] bool parseTrailerLine(std::string_view line);

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
         * @brief 统一的失败记录：置粘滞错误态，并记下类别与中文详情
         * @param errorKind 失败类别
         * @param message 中文错误详情
         */
        void recordFailure(HttpParseErrorKind errorKind, std::string message);

        /**
         * @brief 头部块结束：裁决定界头并决定直接完成还是转入正文阶段
         * @details Transfer-Encoding 与 Content-Length 并存判错、非唯一的 chunked 判错，其余情况按
         *          「有 Content-Length 收定长正文 / 有 chunked 进分块解码 / 都没有即完成」三选一。
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
        std::size_t m_receivedBodyLength{0};   ///< 已收正文字节数（分块时指解码后的字节数）
        std::size_t m_headerFieldCount{0};     ///< 已解析头部条数
        std::size_t m_headerBlockLength{0};    ///< 头部块净字节数，只算名与值，不含 ": " 与 CRLF
        bool        m_hasContentLength{false}; ///< 是否已见过 Content-Length（用于比对重复值）

        /// 是否已见过 Transfer-Encoding：与 Content-Length 互斥，两者并存当场判错
        bool m_hasTransferEncoding{false};

        /// 各条 Transfer-Encoding 取值原文，按到达顺序以 ", " 连接，头部块结束时统一裁决
        std::string m_transferEncodingValue;

        std::size_t m_chunkRemainingBytes{0};      ///< 当前分块尚未收到的块数据字节数
        std::size_t m_chunkTerminatorBytesSeen{0}; ///< 块数据之后已收到的 CRLF 字节数（0 或 1）

        bool        m_hasError{false};        ///< 是否已发生解析错误
        HttpParseErrorKind m_errorKind{HttpParseErrorKind::None}; ///< 失败类别（决定上层回哪个状态码）
        std::string m_errorMessage;           ///< 面向使用者的中文错误描述
        std::size_t m_consumedByteCount{0};   ///< 最近一次 parse() 实际消费的字节数

        /// 方法原文上限 32 B：llhttp 同档取值，通用方法最长 7 B（OPTIONS），留足自定义动词余地。
        /// 它是协议语法约束而不是按部署调整的内存闸门，因此不放进 HttpParserLimits；
        /// kRequestLineFixedOverheadBytes 以它为下限，改本值要同步那一处，否则合法的最长方法会撞整行上限
        static constexpr std::size_t kMaximumMethodLength = 32;

        /// 构造时按值落定的资源上限，收字节与定界时逐项消费；没有中途更换的入口
        HttpParserLimits m_limits{};
    };
} // namespace AsynGyanis::Net
