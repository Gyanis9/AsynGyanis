/**
 * @file JsonReader.h
 * @brief JSON 流式（SAX）事件读取器：push 喂入 / pull 取事件，支持跨分片边界的 token
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Format/Json/JsonParseOptions.h"
#include "Base/Format/TextPosition.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Base
{
    /**
     * @brief JSON 事件类型
     *
     * @details 覆盖 RFC 8259 的全部结构：流边界（StreamStart/StreamEnd）、容器边界
     *          （ObjectStart/ObjectEnd/ArrayStart/ArrayEnd）与标量（Key/String/Number/Bool/Null）。
     *          事件类型一望即知地分成两类：
     *          - `Key` 专用于对象成员名（不额外发 String 事件），消费方据此区分「键」与「字符串值」；
     *          - `Number`/`Bool`/`Null` 只携带原文，类型判定留给消费方（DOM 前端会按
     *            JsonParser 的规则把它解析成 Int/UInt/Double）。
     *
     *          消费方按「开始/结束」成对入栈即可重建文档结构，无需自行处理空白、分隔符与转义。
     */
    enum class JsonEventType : std::uint8_t
    {
        StreamStart, ///< 流开始：首次取事件时发出，早于任何 token
        StreamEnd,   ///< 流结束：finish() 后文档完整结束才发出，之后不再有事件
        ObjectStart, ///< 对象开始：对应 '{'
        ObjectEnd,   ///< 对象结束：对应 '}'
        ArrayStart,  ///< 数组开始：对应 '['
        ArrayEnd,    ///< 数组结束：对应 ']'
        Key,         ///< 对象成员名：text 为反转义后的键
        String,      ///< 字符串值：text 为反转义后的内容
        Number,      ///< 数字：text 为数字原文（如 "-1.5e3"），未做类型判定
        Bool,        ///< true/false：text 为 "true" 或 "false"
        Null         ///< null：text 为空
    };

    /**
     * @brief 单条 JSON 解析事件
     *
     * @details 事件自持文本（std::string），因此可以脱离输入缓冲独立存放与排队。
     *          `position` 是事件在**原始输入流**中的起始位置（行、列、字节偏移），
     *          跨 feed 分片时偏移量按累计字节数计算，与一次性喂入完全一致。
     */
    struct JsonEvent
    {
        JsonEventType  type{JsonEventType::StreamEnd}; ///< 事件类型
        std::string    text;                           ///< 文本载荷，含义随类型而定（见 JsonEventType）
        TextPosition position;                       ///< 事件在输入中的起始位置

        /**
         * @brief 判断是否为容器开始事件
         * @return true 事件为 ObjectStart 或 ArrayStart
         */
        [[nodiscard]] bool isContainerStart() const noexcept
        {
            return type == JsonEventType::ObjectStart || type == JsonEventType::ArrayStart;
        }

        /**
         * @brief 判断是否为容器结束事件
         * @return true 事件为 ObjectEnd 或 ArrayEnd
         */
        [[nodiscard]] bool isContainerEnd() const noexcept
        {
            return type == JsonEventType::ObjectEnd || type == JsonEventType::ArrayEnd;
        }
    };

    /**
     * @brief JSON 流式事件读取器
     *
     * @details 与 DOM 版 JsonParser 的关系与取舍：
     *          - JsonParser 一次性吃下完整文本并直接组装 FormatValue，写法简单、可随机访问结果，
     *            适合配置文件这类「小、要反复按路径取值」的场景；
     *          - JsonReader 只吐事件、不建 DOM，因此**内存占用与文档大小无关**（只与嵌套深度成正比），
     *            可以边收网络分片边消费，适合大文档或流式管道；
     *          - 两者共用同一套选项（JsonParseOptions）与同一套错误分类（FormatErrorKind），
     *            但**不是同一份扫描器**：JsonParser 的递归下降实现依赖「全文可见」，
     *            无法直接改造成可中断的增量扫描，因此 JsonReader 用可恢复状态机独立实现，
     *            两边的语法规则、上限判定与宽松开关保持一致（见 .cpp 中逐条对照的注释）。
     *
     * 两种使用方式：
     *          - pull：构造后直接 nextEvent()（内部输入由 feed() 逐步提供）；
     *          - push：feed() 逐片喂入，喂完调用 finish()，其间可随时 nextEvent() 取事件。
     *          `nextEvent()` 返回空 optional 表示「当前缓冲里没有完整事件」——既可能是
     *          等待更多输入，也可能是流已结束，需结合 isFinished() 与 hasNext() 判断。
     *
     * 跨边界安全：token 扫描始终从 token 起点重新开始，只要当前缓冲不足以判定 token
     *          是否结束就原样返回、不提交任何状态，因此任意切分方式（1 字节、3 字节、随机）
     *          都会得到与一次性喂入完全一致的事件序列。代价是同一 token 被反复扫描，
     *          极端情况下（长字符串按 1 字节喂）复杂度为 O(长度²)；实际网络分片通常为
     *          KiB~MiB 量级，该代价可接受。
     *
     * @note 本读取器始终持有输入副本（分片追加需要），不提供零拷贝视图模式。
     */
    class JsonReader
    {
    public:
        /**
         * @brief 构造读取器
         * @param options 解析选项快照（安全上限与宽松语法开关）
         */
        explicit JsonReader(const JsonParseOptions &options = JsonParseOptions{});

        /**
         * @brief 析构函数
         */
        ~JsonReader() = default;

        /**
         * @brief 移动构造函数
         * @details 转移缓冲与扫描状态；源对象退化为可析构但不可再用。
         * @param other 待移源的读取器
         */
        JsonReader(JsonReader &&other) noexcept = default;

        /**
         * @brief 移动赋值运算符
         * @details 释放自身状态后接管源对象；自移动安全。
         * @param other 待移源的读取器
         * @return JsonReader& 本对象引用
         */
        JsonReader &operator=(JsonReader &&other) noexcept = default;

        // 读取器持有输入缓冲与扫描游标，拷贝会产生两份互不相干的流状态，语义不清晰，故禁止
        JsonReader(const JsonReader &) = delete;
        JsonReader &operator=(const JsonReader &) = delete;

        /**
         * @brief push 模式：喂入一段输入
         * @details 分片追加到内部缓冲，可在任意字节位置切断（token、字符串、转义、数字都可跨片）。
         *          已取过事件后仍可继续喂入，这正是增量解析的用途。
         * @param chunk 待追加的文本片段
         * @throws FormatError 累计输入超出 maximumInputLength
         */
        void feed(std::string_view chunk);

        /**
         * @brief 声明输入结束
         * @details 幂等；调用后未闭合的容器、未收尾的字符串/转义才会被判为错误，
         *          在此之前一律按「等待更多输入」处理。
         */
        void finish();

        /**
         * @brief 判断当前是否还能取到事件
         * @details 与 nextEvent() 基于同一套判定，不会抛出「需要更多输入」以外的错误；
         *          但若缓冲中存在语法错误，本函数仍可能抛 FormatError（与 nextEvent 一致）。
         * @return true 当前缓冲中已有一个可完整判定的事件
         * @throws FormatError 扫描期发现语法错误
         */
        [[nodiscard]] bool hasNext();

        /**
         * @brief 取出下一条事件
         * @return std::optional<JsonEvent> 下一条事件；当前缓冲不足以判定或流已结束返回空
         * @throws FormatError 输入为空、语法非法、超出选项上限或容器未闭合
         */
        [[nodiscard]] std::optional<JsonEvent> nextEvent();

        /**
         * @brief 一次性读取全部事件
         * @details 便捷入口，等价于「构造 → feed 全文 → finish → 反复 nextEvent」，
         *          适合小文档与需要随机访问事件流的场合；大文档请直接用 feed/nextEvent。
         * @param text 完整 JSON 文本
         * @param options 解析选项
         * @return std::vector<JsonEvent> 完整事件序列（含 StreamStart/StreamEnd）
         * @throws FormatError 语法非法或超出选项上限
         */
        [[nodiscard]] static std::vector<JsonEvent> readAll(std::string_view text, const JsonParseOptions &options = JsonParseOptions{});

        /**
         * @brief 获取解析选项快照
         * @return const JsonParseOptions& 构造时确定的选项
         */
        [[nodiscard]] const JsonParseOptions &options() const noexcept;

        /**
         * @brief 判断输入是否已声明结束
         * @return true 已调用过 finish()
         */
        [[nodiscard]] bool isFinished() const noexcept;

    private:
        /**
         * @brief 一次 token 扫描的结果
         */
        enum class ScanResult : std::uint8_t
        {
            Complete, ///< 已完整判定，可以提交
            NeedMore  ///< 缓冲在此处截断，需更多输入后从 token 起点重扫
        };

        /**
         * @brief 扫描状态机的阶段
         *
         * @details 根阶段（RootValue/RootEnd）与容器内阶段共用同一个枚举；
         *          栈非空时以栈顶容器的阶段为准，栈空时以 m_rootPhase 为准。
         */
        enum class ScanPhase : std::uint8_t
        {
            RootValue,        ///< 期待文档根值
            RootEnd,          ///< 根值已完成，只允许空白（其后任何内容都是 TrailingContent）
            ArrayFirst,       ///< 数组 '[' 之后：值或 ']'
            ArrayElement,     ///< 数组 ',' 之后：值（宽松模式允许直接跟 ']'）
            ArrayDelimiter,   ///< 数组元素之后：',' 或 ']'
            ObjectFirstKey,   ///< 对象 '{' 之后：键或 '}'
            ObjectKey,        ///< 对象 ',' 之后：键（宽松模式允许直接跟 '}'）
            ObjectColon,      ///< 键之后：':'
            ObjectValue,      ///< ':' 之后：值
            ObjectDelimiter   ///< 成员值之后：',' 或 '}'
        };

        /**
         * @brief 一个未闭合容器的扫描状态
         */
        struct ContainerFrame
        {
            bool      isObject{false};     ///< true 为对象、false 为数组
            ScanPhase phase{ScanPhase::ArrayFirst}; ///< 当前阶段
            std::size_t elementCount{0};   ///< 已确认的元素/成员个数（用于个数上限与尾逗号判定）
        };

        /**
         * @brief 跳过空白与（可选）注释，把游标停在 token 起点
         * @details 注释与空白都以「能完整判定」为前提才提交游标，遇到缓冲截断时原样返回。
         * @return true 已停在 token 起点或输入结束；false 需要更多输入
         * @throws FormatError 块注释未闭合（仅在输入已结束时可能发生）
         */
        [[nodiscard]] bool ensureScanReady();

        /**
         * @brief 真正推进状态机并产出下一条事件
         * @details nextEvent() 与 hasNext() 的共同实现；hasNext() 会把结果放进单槽前瞻，
         *          因此探测不会丢失事件。
         * @return std::optional<JsonEvent> 下一条事件；需要更多输入或流已结束时返回空
         * @throws FormatError 语法非法、超出选项上限或容器未闭合
         */
        [[nodiscard]] std::optional<JsonEvent> produceNextEvent();

        /**
         * @brief 判定并跳过输入起始的 UTF-8 BOM
         * @details BOM 可能被切成 2 片，因此不足 3 字节且是 BOM 前缀时要求更多输入。
         * @return true 已判定完毕；false 需要更多输入
         */
        [[nodiscard]] bool resolveUtf8Bom();

        /**
         * @brief 扫描一个字符串 token（含首尾引号）
         * @param startIndex 起始引号所在偏移
         * @param decodedText 输出参数，反转义后的文本
         * @param endIndex 输出参数，收尾引号之后的偏移
         * @return ScanResult 扫描结果
         * @throws FormatError 字符串未闭合、含裸控制字符、转义非法或超出长度上限
         */
        [[nodiscard]] ScanResult scanString(std::size_t startIndex, std::string &decodedText, std::size_t &endIndex) const;

        /**
         * @brief 扫描一个数字 token
         * @param startIndex 数字首字节偏移
         * @param rawText 输出参数，数字原文
         * @param endIndex 输出参数，token 之后的偏移
         * @return ScanResult 扫描结果
         * @throws FormatError 数字语法非法或超出可表示范围
         */
        [[nodiscard]] ScanResult scanNumber(std::size_t startIndex, std::string &rawText, std::size_t &endIndex) const;

        /**
         * @brief 扫描 true/false/null 字面量
         * @param startIndex 字面量首字节偏移
         * @param keyword 输出参数，字面量文本
         * @param endIndex 输出参数，token 之后的偏移
         * @return ScanResult 扫描结果
         * @throws FormatError 字面量拼写或后缀非法
         */
        [[nodiscard]] ScanResult scanKeyword(std::size_t startIndex, std::string &keyword, std::size_t &endIndex) const;

        /**
         * @brief 扫描一段注释
         * @details 仅当输入已结束时才把未闭合的块注释判为错误，否则视为「需要更多输入」。
         * @param startIndex 注释引导符 '/' 所在偏移
         * @param endIndex 输出参数，注释之后的偏移
         * @return ScanResult 扫描结果
         * @throws FormatError 块注释在输入结束时仍未闭合
         */
        [[nodiscard]] ScanResult scanComment(std::size_t startIndex, std::size_t &endIndex) const;

        /**
         * @brief 校验字符串正文中的 UTF-8 字节序列
         * @details 与 JsonParser 使用同一套 RFC 3629 规则（拒绝过长编码、越界码位、
         *          UTF-8 形式的代理项与截断序列）；因 JsonParser 的校验是私有实现，
         *          此处按同一规则独立实现，后续可上收为 TextEscapes 共用原语。
         * @param bodyStart 正文起始偏移
         * @param bodyEnd 正文结束偏移（不含）
         * @param bodyStartPosition 正文起点位置，用于按字节偏移换算列号
         * @throws FormatError 出现非法或截断的 UTF-8 序列
         */
        void validateStringUtf8(std::size_t bodyStart, std::size_t bodyEnd, const TextPosition &bodyStartPosition) const;

        /**
         * @brief 扫描一个值（标量或容器）并产出对应事件
         * @return std::optional<JsonEvent> 事件；缓冲截断时返回空
         * @throws FormatError 语法非法或超出上限
         */
        [[nodiscard]] std::optional<JsonEvent> scanValueEvent();

        /**
         * @brief 扫描一个对象成员名并产出 Key 事件
         * @return std::optional<JsonEvent> Key 事件；缓冲截断时返回空
         * @throws FormatError 键不是字符串、转义非法或超出上限
         */
        [[nodiscard]] std::optional<JsonEvent> scanKeyEvent();

        /**
         * @brief 进入一个容器并产出开始事件
         * @details 先把父级的「一个值已完成」状态落地，再压栈；深度超限在此抛错。
         * @param isObject true 表示对象、false 表示数组
         * @throws FormatError 嵌套深度超出 maximumDepth
         */
        void beginContainer(bool isObject);

        /**
         * @brief 结束栈顶容器并产出结束事件
         * @return JsonEvent 结束事件
         * @throws FormatError 栈为空（内部状态异常）
         */
        [[nodiscard]] JsonEvent closeContainer();

        /**
         * @brief 记录「父级的一个值已完成」
         * @details 数组会递增元素计数；根值为容器时不动根阶段（等弹栈时再置 RootEnd），
         *          否则把父级阶段推进到分隔符位置。
         * @param isContainerStart 该值是否为容器开始
         * @throws FormatError 元素个数超出 maximumContainerElements
         */
        void completeValueInParent(bool isContainerStart);

        /**
         * @brief 递增栈顶容器的元素/成员计数并校验上限
         * @throws FormatError 个数超出 maximumContainerElements
         */
        void countElement();

        /**
         * @brief 获取当前阶段
         * @return ScanPhase 栈非空时为栈顶容器阶段，否则为根阶段
         */
        [[nodiscard]] ScanPhase currentPhase() const noexcept;

        /**
         * @brief 设置当前阶段
         * @param phase 新阶段；栈非空时作用于栈顶容器，否则作用于根阶段
         */
        void setCurrentPhase(ScanPhase phase) noexcept;

        /**
         * @brief 提交游标到指定偏移并同步行列号
         * @param endIndex 新的游标位置（不得小于当前游标）
         */
        void advanceTo(std::size_t endIndex) noexcept;

        /**
         * @brief 读取指定偏移的字节
         * @param index 字节偏移
         * @return char 该字节；越界返回 '\0'
         */
        [[nodiscard]] char peekByte(std::size_t index) const noexcept;

        /**
         * @brief 获取游标处的位置
         * @return TextPosition 行、列与字节偏移
         */
        [[nodiscard]] TextPosition currentPosition() const noexcept;

        /**
         * @brief 获取任意偏移处的位置（用于错误定位）
         * @param index 目标字节偏移
         * @return TextPosition 由当前游标位置向前推演得到的位置
         */
        [[nodiscard]] TextPosition positionAt(std::size_t index) const noexcept;

        std::string       m_buffer;                    ///< 已喂入的输入缓冲（分片追加）
        std::size_t       m_cursor{0};                 ///< 下一个待扫描字节，始终停在 token 起点
        std::size_t       m_line{1};                   ///< 游标所在行号，从 1 起始
        std::size_t       m_column{1};                 ///< 游标所在列号，从 1 起始
        JsonParseOptions  m_options;                   ///< 解析选项快照
        bool              m_finished{false};           ///< 是否已声明输入结束
        bool              m_streamStarted{false};      ///< 是否已发出 StreamStart
        bool              m_streamEnded{false};        ///< 是否已发出 StreamEnd
        bool              m_bomResolved{false};        ///< 输入起始的 BOM 是否已判定完毕
        ScanPhase         m_rootPhase{ScanPhase::RootValue}; ///< 根阶段（栈空时生效）
        std::vector<ContainerFrame> m_stack;           ///< 未闭合容器栈，栈顶即当前容器
        std::optional<JsonEvent>    m_pending;         ///< 单槽前瞻事件，供 hasNext() 探测而不消费
    };
} // namespace AsynGyanis::Base
