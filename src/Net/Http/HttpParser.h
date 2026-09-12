/**
 * @file HttpParser.h
 * @brief HTTP/1.1 请求报文增量解析器，封装 llhttp C 库
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Net/Http/HttpRequest.h"
#include "Net/Http/ParseStatus.h"

#include <llhttp.h>

#include <cstddef>
#include <string>

namespace AsynGyanis::Net
{
    /**
     * @brief HTTP/1.1 请求报文解析器，基于 llhttp 状态机
     *
     * @details 以增量方式解析请求：每次把新读到的字节交给 parse()，解析过程中通过
     *          llhttp 回调把 URI、头部、正文逐段填进内部的 HttpRequest，完成后可经
     *          request() 读出。两条报文之间必须调用 reset() 交还一个干净的请求对象
     *          （HttpSession 的 keep-alive 循环已这么做）；解析器自身的暂存缓冲在
     *          llhttp 的 on_message_begin 里就会归零，不依赖上层记得复位。
     *
     * @note 与 llhttp 的契约：回调类型是 C 函数指针，因此全部实现为 static 成员函数，
     *       不捕获 this，宿主对象统一经 llhttp_t::data 取回；回调返回值非 0 会让
     *       llhttp 立即中断本次解析，并把该返回值作为 llhttp_execute() 的结果交回调用方。
     *       本解析器的回调只用到三种返回值：
     *       @li 0 —— 继续解析；
     *       @li HPE_USER —— 撞上资源上限，配合 llhttp_set_error_reason() 中断本次解析；
     *       @li -1 —— 仅用于 on_message_begin 的流水线守卫（上一条还没 reset 就开了下一条），
     *           此时完成标记已置位，parse() 仍按 Done 对外呈现，不会误报成解析错误。
     *       不使用 llhttp_pause()，也不在回调里返回 HPE_PAUSED——llhttp 明确要求不要在回调里
     *       调用 pause，而暂停在不同版本上的处理路径不一致，不适合当流控手段。
     *
     * @warning 所有资源上限都是 DoS 防护：任何一项超限都会以 Error 结束本次解析，
     *          并用 isLimitExceeded() 标出「超限」这一子类，便于上层回 431/413 而不是 400。
     *          上限是策略而非协议要求，取值依据见各常量的行尾注释。
     */
    class HttpParser
    {
    public:
        /**
         * @brief 构造解析器，初始化 llhttp 设置表并把回调挂到本对象上。
         */
        HttpParser();

        /**
         * @brief 析构函数。
         * @details 解析器与设置表都是按值持有的 POD，没有堆资源需要回收；
         *          llhttp 不会反向调用 data，因此也不存在悬垂注册。
         */
        ~HttpParser();

        // 禁拷贝也禁移动：llhttp 的设置表里登记了指向本对象的 data，
        // 一旦复制或用移动构造出第二个实例，回调就会认到错误的宿主对象上，
        // 这类错误只会在运行期以「改错对象」的形式暴露，代价远高于放弃值语义
        HttpParser(const HttpParser &)            = delete;
        HttpParser &operator=(const HttpParser &) = delete;

        /**
         * @brief 解析一段输入数据。
         *
         * @details 状态判定可推理，三步互斥且穷尽：
         *          @li 先看完成标记：on_message_complete 触发过就返回 Done，且不再消耗任何字节
         *              （多余字节属于流水线里的下一条报文，喂进已完成的解析器会破坏上一条结果）；
         *          @li 再看 llhttp 返回码：HPE_OK 且未完成 → NeedMore，表示报文还没收齐；
         *          @li 其余返回码 → Error，含报文非法与超限（回调主动返回 HPE_USER）两类，
         *              后者由 isLimitExceeded() 区分。
         *          完成与否只以 on_message_complete 为准：HPE_OK 仅代表「本段字节合法且状态机没走完」，
         *          绝不代表解析结束；同一次调用里完成与错误同时出现时按完成处理并丢弃剩余字节。
         *
         * @param data   数据起始指针，调用方保证可读
         * @param length 数据长度，单位字节
         * @retval ParseStatus::Done 一条完整报文收齐，request() 的全部内容此刻才可读
         * @retval ParseStatus::NeedMore 仍需更多数据；此时 request() 是半成品，
         *                               method/uri/httpVersion 尚未填充（它们在完成回调里才定稿），
         *                               已入库的头部与正文可读但随时可能被追加，不得当作最终结果
         * @retval ParseStatus::Error 解析失败，request() 只能视为不可信并丢弃；
         *                            解析器进入粘滞错误态，除非 reset()，后续调用仍返回 Error
         * @see ParseStatus, isLimitExceeded(), errorMessage()
         */
        ParseStatus parse(const char *data, size_t length);

        /**
         * @brief 重置解析器状态，以便解析下一条消息。
         * @details 清空请求对象与全部暂存缓冲、错误标记，并重新初始化 llhttp 状态机
         *          ——只有重新初始化才能摆脱 llhttp 的错误粘滞。
         */
        void reset();

        /**
         * @brief 获取当前解析出的 HTTP 请求对象引用。
         * @details 仅在 parse() 返回 Done 后内容完整可用。
         * @return HttpRequest& 引用，生命周期跟随本解析器
         */
        HttpRequest &request();

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
         * @return 面向使用者的中文错误文本；无错误时为空串。
         *         llhttp 自身的英文 reason 会被包进中文外壳一并给出
         */
        [[nodiscard]] std::string errorMessage() const;

    private:
        /**
         * @brief 从 llhttp 解析器实例取回宿主对象
         * @param parser 触发回调的解析器实例
         * @return 宿主解析器指针；构造与 reset() 都紧跟 llhttp_init 重新绑定过 data，故必非空
         */
        static HttpParser *ownerFrom(llhttp_t *parser) noexcept;

        // 以下均为 llhttp 回调：签名受 C 函数指针约束，必须是 static 成员函数，返回非 0 即中断解析
        static int onMessageBegin(llhttp_t *parser);                                 ///< 一条报文开始的回调，归零本条暂存
        static int onUrl(llhttp_t *parser, const char *data, size_t length);         ///< URI 分片回调
        static int onHeaderField(llhttp_t *parser, const char *data, size_t length); ///< 头部名分片回调
        static int onHeaderValue(llhttp_t *parser, const char *data, size_t length); ///< 头部值分片回调
        static int onHeaderValueComplete(llhttp_t *parser);                          ///< 头部值收尾回调，一条头部在此落库
        static int onBody(llhttp_t *parser, const char *data, size_t length);        ///< 正文分片回调
        static int onMessageComplete(llhttp_t *parser);                              ///< 报文完成回调，方法/URI/版本在此定稿

        /**
         * @brief 清空单条报文的暂存：URI、头部名与值的缓冲，以及头部条数与总长计数
         */
        void clearMessageScratch() noexcept;

        /**
         * @brief 记录一次「资源上限」失败
         * @param parser 当前解析器，用于向 llhttp 登记错误原因
         * @param llhttpReason 交给 llhttp 的英文原因，必须是静态期生命周期的字面量
         * @param detailMessage 面向使用者的中文详情（含上限数值）
         */
        void recordResourceLimitExceeded(llhttp_t *parser, const char *llhttpReason, std::string detailMessage);

        /**
         * @brief 把 llhttp 的错误码翻译成使用者可读的中文记录
         * @param executionResult llhttp_execute() 返回的错误码
         */
        void recordParseError(llhttp_errno_t executionResult);

        llhttp_t m_parser{};              ///< llhttp 解析器实例，按值持有，无堆资源
        llhttp_settings_t m_settings{};   ///< llhttp 回调设置表，生命周期必须不短于解析器
        HttpRequest m_currentRequest;     ///< 当前正在构建的 HTTP 请求
        std::string m_currentUrl;         ///< 累积中的请求 URI，可能跨多次回调分片到达
        std::string m_currentHeaderField; ///< 累积中的头部名，可能跨多次回调分片到达
        std::string m_currentHeaderValue; ///< 累积中的头部值，可能跨多次回调分片到达
        std::size_t m_headerFieldCount = 0;  ///< 本条报文已落库的头部条数
        std::size_t m_headerBlockLength = 0; ///< 本条报文头部块的净字节数，只算名与值，不含 ": " 与 CRLF
        bool m_hasError = false;          ///< 是否已发生解析错误
        bool m_isComplete = false;        ///< 是否已收完整条报文，完成的唯一判据
        bool m_isLimitExceeded = false;   ///< 错误是否由资源上限触发
        std::string m_errorMessage;       ///< 面向使用者的中文错误描述

        // 资源上限：全部按「正常流量远达不到、恶意流量立刻撞线」的口径取值，单位统一为字节。
        // 任何一项超限都走 Error + isLimitExceeded()，绝不静默截断后继续解析
        static constexpr std::size_t kMaximumBodySize = 8ull * 1024 * 1024;        ///< 请求体上限 8 MiB：够上传小文件，不够拖垮内存，与限流中间件的默认档位一致
        static constexpr std::size_t kMaximumUriLength = 8ull * 1024;              ///< 请求 URI 上限 8 KiB：对齐 nginx large_client_header_buffers 的单行 8 KiB，浏览器实际 URI 远低于此
        static constexpr std::size_t kMaximumHeaderFieldNameLength = 256;          ///< 单个头部名上限 256 B：标准头部名最长不过数十 B，留足私有前缀（x-amz- 等）后仍宽裕
        static constexpr std::size_t kMaximumHeaderFieldValueLength = 8ull * 1024; ///< 单个头部值上限 8 KiB：与 URI 同档，覆盖超长 Cookie 头的现实用量
        static constexpr std::size_t kMaximumHeaderCount = 100;                    ///< 头部条数上限 100 条：浏览器实际请求不足 40 条，此值专防「海量空值头部」撑爆容器节点
        static constexpr std::size_t kMaximumHeaderBlockLength = 64ull * 1024;     ///< 头部块总长上限 64 KiB：名与值净字节之和，条数与单条之外的第三道闸，约合 8 个 8 KiB 接收缓冲区
    };
} // namespace AsynGyanis::Net
