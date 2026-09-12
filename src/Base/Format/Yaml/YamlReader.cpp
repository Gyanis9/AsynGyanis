/**
 * @file YamlReader.cpp
 * @brief YAML 流式事件读取器实现：完整的 YAML 1.2 词法与块结构扫描器
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Format/Yaml/YamlReader.h"

#include "Base/Format/FormatError.h"
#include "Base/Format/TextEscapes.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /// 核心 schema 标签前缀（`!!` 默认句柄展开目标），对应 YAML 1.2 §6.8.2
        constexpr std::string_view kCoreTagPrefix = "tag:yaml.org,2002:";

        /// 行分隔符，用于块标量折叠与 chomping 计数
        constexpr char kLineBreak = '\n';

        /**
         * @brief 统计文本前导空格数量
         * @param text 待统计文本
         * @return std::size_t 连续空格个数
         */
        std::size_t countLeadingSpaces(std::string_view text) noexcept
        {
            const std::size_t position = text.find_first_not_of(' ');
            return position == std::string_view::npos ? text.size() : position;
        }

        /**
         * @brief 去掉尾部空白（空格与制表符）
         * @param text 待裁剪文本
         * @return std::string_view 裁剪后的视图
         */
        std::string_view trimRight(std::string_view text) noexcept
        {
            const std::size_t position = text.find_last_not_of(" \t");
            return position == std::string_view::npos ? std::string_view{} : text.substr(0, position + 1);
        }

        /**
         * @brief 去掉首尾空白（空格与制表符）
         * @param text 待裁剪文本
         * @return std::string_view 裁剪后的视图
         */
        std::string_view trim(std::string_view text) noexcept
        {
            const std::size_t begin = text.find_first_not_of(" \t");
            if (begin == std::string_view::npos)
            {
                return {};
            }
            const std::size_t end = text.find_last_not_of(" \t");
            return text.substr(begin, end - begin + 1);
        }

        /**
         * @brief 判断是否为全空白行
         * @param raw 原始行
         * @return true 行内只有空格与制表符
         */
        bool isBlankLine(std::string_view raw) noexcept
        {
            return raw.find_first_not_of(" \t") == std::string_view::npos;
        }

        /**
         * @brief 判断正文是否以块序列条目开头（YAML 1.2 §8.2.1 的 `-` 指示符）
         * @param content 去掉缩进后的正文
         * @return true 形如 `-`（行尾）或 `- ...`
         */
        bool startsSequenceEntry(std::string_view content) noexcept
        {
            return content == "-" || (content.size() > 1 && content.front() == '-' && content[1] == ' ');
        }

        /**
         * @brief 判断是否是显式复杂键指示符（YAML 1.2 §8.2.2 的 `?`）
         * @param content 去掉缩进后的正文
         * @return true 形如 `?`（行尾）或 `? ...`
         */
        bool startsExplicitKey(std::string_view content) noexcept
        {
            return content == "?" || (content.size() > 1 && content.front() == '?' && content[1] == ' ');
        }

        /**
         * @brief 判断是否文档起始标记
         * @param content 已裁掉注释的正文
         * @return true 形如 `---` 或 `--- ...`
         */
        bool isDocumentStartMarker(std::string_view content) noexcept
        {
            return content == "---" || (content.size() > 3 && content.substr(0, 3) == "---" && content[3] == ' ');
        }

        /**
         * @brief 判断是否文档结束标记
         * @param content 已裁掉注释的正文
         * @return true 形如 `...` 或 `... ...`
         */
        bool isDocumentEndMarker(std::string_view content) noexcept
        {
            return content == "..." || (content.size() > 3 && content.substr(0, 3) == "..." && content[3] == ' ');
        }

        /**
         * @brief 去掉行尾注释（`#` 且前置空白或位于行首才构成注释，YAML 1.2 §6.2）
         * @param content 行正文（含可能的注释）
         * @return std::string_view 注释之前的正文
         */
        std::string_view stripTrailingComment(std::string_view content) noexcept
        {
            for (std::size_t index = 0; index < content.size(); ++index)
            {
                if (content[index] != '#')
                {
                    continue;
                }
                // 只有行首或前置空白时 '#' 才是注释引导符，`a#b` 属于标量正文
                if (index == 0 || content[index - 1] == ' ' || content[index - 1] == '\t')
                {
                    return trimRight(content.substr(0, index));
                }
            }
            return trimRight(content);
        }

        /**
         * @brief 计算十六进制数字的数值
         * @param character 待判定字符
         * @return int 数值，非十六进制字符返回 -1
         */
        int hexDigitValue(const char character) noexcept
        {
            if (character >= '0' && character <= '9')
            {
                return character - '0';
            }
            if (character >= 'a' && character <= 'f')
            {
                return character - 'a' + 10;
            }
            if (character >= 'A' && character <= 'F')
            {
                return character - 'A' + 10;
            }
            return -1;
        }

        /**
         * @brief 节点属性（锚点与显式标签），对应 YAML 1.2 §3.2.1
         */
        struct NodeProperties
        {
            std::string anchor; ///< `&name` 锚点名，空表示未声明
            std::string tag;    ///< 展开后的显式标签，空表示未声明
        };

        /**
         * @brief 一行原始文本及其结构属性
         */
        struct Line
        {
            std::string_view raw;      ///< 不含行尾换行的原始行
            std::string_view content;  ///< 去掉前导缩进后的正文（可能含行尾注释）
            std::size_t      indent{0};///< 前导空格数
            std::size_t      number{1};///< 1 基行号
            std::size_t      offset{0};///< 原始行在输入中的字节偏移
            bool             structural{false}; ///< 是否参与结构（非空且非整行注释）
        };

        /**
         * @brief 块标量的 chomping 指示（YAML 1.2 §8.1.1.2）
         */
        enum class ChompingMode : std::uint8_t
        {
            Clip,  ///< 默认：保留一个尾随换行
            Strip, ///< `-`：去掉所有尾随换行
            Keep   ///< `+`：保留所有尾随换行
        };

        /**
         * @brief 块标量头部解析结果
         */
        struct BlockScalarHeader
        {
            bool         isFolded{false};              ///< true 为 `>`（折叠），false 为 `|`（字面）
            ChompingMode chomping{ChompingMode::Clip}; ///< chomping 指示
            std::size_t  explicitIndent{0};            ///< 显式缩进位数，0 表示自动探测
        };

        /**
         * @brief 流式容器解析游标
         */
        struct FlowCursor
        {
            std::string_view text;      ///< 归一化后的流式文本
            std::size_t      index{0};  ///< 当前偏移
            std::size_t      line{1};   ///< 当前行号（近似，用于错误定位）
            std::size_t      column{1}; ///< 当前列号（近似）
        };
    } // namespace

    /**
     * @brief YAML 扫描器实现
     *
     * @details 单一递归下降扫描器：块结构、标量风格、指令、标签与锚点全部在此识别并
     *          产出 YamlEvent；DOM 前端 YamlParser 只消费事件，两个前端共享本实现。
     */
    class YamlReaderImplementation
    {
    public:
        std::string_view m_input;        ///< 输入视图（零拷贝或指向 m_ownedInput）
        std::string      m_ownedInput;   ///< push 模式下的自有缓冲
        YamlParseOptions m_options;      ///< 解析选项
        bool             m_finished{false}; ///< push 模式是否已声明输入结束
        bool             m_scanned{false};  ///< 是否已完成扫描
        bool             m_consuming{false};///< 是否已开始取事件（此后禁止再 feed）

        std::vector<YamlEvent>   m_events;      ///< 扫描产生的事件序列
        std::size_t              m_eventIndex{0};///< 下一个待取事件下标
        std::vector<std::string> m_warnings;    ///< 非致命警告

        std::vector<Line> m_lines;           ///< 拆分后的行
        std::size_t       m_lineIndex{0};    ///< 当前行下标
        std::size_t       m_depth{0};        ///< 当前节点嵌套深度
        std::size_t       m_documentCount{0};///< 已扫描文档数

        std::map<std::string, std::string, std::less<> > m_tagHandles; ///< `%TAG` 句柄表

        /**
         * @brief 深度计数守卫：构造时递增、析构时递减
         */
        class DepthGuard
        {
        public:
            /**
             * @brief 绑定深度计数器并递增
             * @param depth 深度计数器引用
             */
            explicit DepthGuard(std::size_t &depth) :
                m_depth(depth)
            {
                ++m_depth;
            }

            /**
             * @brief 析构时恢复深度
             */
            ~DepthGuard()
            {
                --m_depth;
            }

            DepthGuard(const DepthGuard &) = delete;
            DepthGuard &operator=(const DepthGuard &) = delete;

        private:
            std::size_t &m_depth; ///< 被守护的深度计数器
        };

        /**
         * @brief 执行完整扫描（幂等）
         * @throws FormatError 语法非法或超出选项上限
         */
        void run()
        {
            if (m_scanned)
            {
                return;
            }
            m_scanned = true;

            // 输入长度上限：先于建表拦截，避免为超大输入分配行表
            if (m_options.maximumInputLength != 0 && m_input.size() > m_options.maximumInputLength)
            {
                throw FormatError(FormatErrorKind::SizeExceeded,
                                  std::format("输入长度 {} 字节超出上限 {}", m_input.size(), m_options.maximumInputLength),
                                  TextPosition{1, 1, 0});
            }

            splitLines();
            m_tagHandles.emplace("!", "!");
            m_tagHandles.emplace("!!", std::string(kCoreTagPrefix));

            emit(YamlEvent{.type = YamlEventType::StreamStart, .position = TextPosition{1, 1, 0}});
            scanStream();
            emit(YamlEvent{.type = YamlEventType::StreamEnd, .position = TextPosition{1, 1, m_input.size()}});
        }

    private:
        // ---------------------------------------------------------------- 行拆分

        /**
         * @brief 把输入拆分为行，校验制表符缩进并标记结构行
         * @throws FormatError 制表符参与缩进（YAML 1.2 §6.1 明确禁止）
         */
        void splitLines()
        {
            // 跳过 UTF-8 BOM，使后续偏移与列号从有效内容起算
            if (m_input.size() >= 3 && static_cast<unsigned char>(m_input[0]) == 0xEF &&
                static_cast<unsigned char>(m_input[1]) == 0xBB && static_cast<unsigned char>(m_input[2]) == 0xBF)
            {
                m_input = m_input.substr(3);
            }

            m_lines.reserve(m_input.size() / 32 + 1);

            std::size_t       start     = 0;
            std::size_t       number    = 0;
            const std::size_t inputSize = m_input.size();

            // 以 start < inputSize 为界：输入末尾的换行不再制造一行「幻影空行」，
            // 否则 `|+` 之类的 chomping 会把尾随换行数多算一个
            while (start < inputSize)
            {
                ++number;

                const std::size_t lineEnd = m_input.find('\n', start);
                std::size_t       rawEnd  = lineEnd == std::string_view::npos ? inputSize : lineEnd;
                // 兼容 CRLF：行尾 '\r' 不属于正文
                if (rawEnd > start && m_input[rawEnd - 1] == '\r')
                {
                    --rawEnd;
                }

                Line line;
                line.raw    = m_input.substr(start, rawEnd - start);
                line.number = number;
                line.offset = start;

                if (isBlankLine(line.raw))
                {
                    // 全空白行不构成缩进，也就无所谓制表符违规
                    line.indent     = line.raw.size();
                    line.structural = false;
                } else
                {
                    const std::size_t indent = countLeadingSpaces(line.raw);
                    if (line.raw[indent] == '\t')
                    {
                        throw FormatError(FormatErrorKind::UnexpectedByte,
                                          "缩进禁止使用制表符（YAML 1.2 §6.1）",
                                          TextPosition{number, indent + 1, start + indent});
                    }

                    line.indent     = indent;
                    line.content    = line.raw.substr(indent);
                    line.structural = line.content.front() != '#';
                }
                m_lines.push_back(line);

                if (lineEnd == std::string_view::npos)
                {
                    break;
                }
                start = lineEnd + 1;
            }
        }

        // ---------------------------------------------------------------- 事件发射

        /**
         * @brief 追加一条事件
         * @param event 事件对象（按值移动）
         */
        void emit(YamlEvent event)
        {
            m_events.push_back(std::move(event));
        }

        /**
         * @brief 若有锚点属性则先发射独立的 Anchor 事件
         * @param properties 节点属性
         * @param position 节点位置
         */
        void emitAnchorIfNeeded(const NodeProperties &properties, const TextPosition &position)
        {
            if (properties.anchor.empty())
            {
                return;
            }
            YamlEvent anchorEvent;
            anchorEvent.type     = YamlEventType::Anchor;
            anchorEvent.anchor   = properties.anchor;
            anchorEvent.position = position;
            emit(std::move(anchorEvent));
        }

        /**
         * @brief 发射一个标量事件
         * @param text 已解码/折叠的标量文本
         * @param style 标量风格
         * @param properties 节点属性
         * @param position 标量位置
         * @throws FormatError 标量长度超出上限
         */
        void emitScalar(std::string text, const YamlScalarStyle style, const NodeProperties &properties, const TextPosition &position)
        {
            if (m_options.maximumScalarLength != 0 && text.size() > m_options.maximumScalarLength)
            {
                throw FormatError(FormatErrorKind::SizeExceeded,
                                  std::format("单个标量长度 {} 字节超出上限 {}", text.size(), m_options.maximumScalarLength),
                                  position);
            }

            emitAnchorIfNeeded(properties, position);

            YamlEvent scalarEvent;
            scalarEvent.type     = YamlEventType::Scalar;
            scalarEvent.style    = style;
            scalarEvent.text     = std::move(text);
            scalarEvent.tag      = properties.tag;
            scalarEvent.anchor   = properties.anchor;
            scalarEvent.position = position;
            emit(std::move(scalarEvent));
        }

        /**
         * @brief 发射一个空标量（null）事件
         * @param properties 节点属性
         * @param position 位置
         */
        void emitNullScalar(const NodeProperties &properties, const TextPosition &position)
        {
            emitScalar(std::string{}, YamlScalarStyle::Plain, properties, position);
        }

        /**
         * @brief 构造某行某列的位置
         * @param line 行对象
         * @param columnInRaw 行内列偏移（0 基）
         * @return TextPosition 位置对象
         */
        [[nodiscard]] static TextPosition positionOf(const Line &line, const std::size_t columnInRaw) noexcept
        {
            return TextPosition{line.number, columnInRaw + 1, line.offset + columnInRaw};
        }

        /**
         * @brief 构造当前位置（用于深度与流式错误）
         * @return TextPosition 当前位置
         */
        [[nodiscard]] TextPosition currentPosition() const noexcept
        {
            if (m_lineIndex < m_lines.size())
            {
                return positionOf(m_lines[m_lineIndex], m_lines[m_lineIndex].indent);
            }
            return TextPosition{};
        }

        /**
         * @brief 检查嵌套深度是否超限
         * @throws FormatError 超出 maximumDepth
         */
        void checkDepth() const
        {
            if (m_options.maximumDepth == 0 || m_depth <= m_options.maximumDepth)
            {
                return;
            }
            throw FormatError(FormatErrorKind::DepthExceeded,
                              std::format("嵌套深度超出上限 {}", m_options.maximumDepth),
                              currentPosition());
        }

        /**
         * @brief 跳过空行与整行注释
         */
        void skipTrivia() noexcept
        {
            while (m_lineIndex < m_lines.size() && !m_lines[m_lineIndex].structural)
            {
                ++m_lineIndex;
            }
        }

        /**
         * @brief 判断某行是否为**列 0** 的文档起始标记
         * @details §9.1.3 的 `c-directives-end` 是文档起始标记，只有位于列 0 才具备结构含义；
         *          缩进过的 `---` 属于所在节点的内容（例如 §8.1.2 的 l-nb-literal-text）。
         * @param line 待判定行
         * @return true 该行是文档起始标记
         */
        [[nodiscard]] static bool isDocumentStartLine(const Line &line) noexcept
        {
            return line.indent == 0 && isDocumentStartMarker(stripTrailingComment(line.content));
        }

        /**
         * @brief 判断某行是否为**列 0** 的文档结束标记
         * @details §9.1.3 的 `c-document-end` 同样只在列 0 成立，缩进过的 `...` 是普通内容。
         * @param line 待判定行
         * @return true 该行是文档结束标记
         */
        [[nodiscard]] static bool isDocumentEndLine(const Line &line) noexcept
        {
            return line.indent == 0 && isDocumentEndMarker(stripTrailingComment(line.content));
        }

        /**
         * @brief 判断某行是否为**列 0** 的指令行
         * @details §6.8 的指令只出现在文档头且只在列 0；缩进过的 `%` 是普通内容，块标量里
         *          的 `%` 行尤其必须按内容保留（§8.1.2），否则会误报「指令缺少参数」。
         * @param line 待判定行
         * @return true 该行是指令
         */
        [[nodiscard]] static bool isDirectiveLine(const Line &line) noexcept
        {
            if (line.indent != 0)
            {
                return false;
            }
            const std::string_view content = stripTrailingComment(line.content);
            return !content.empty() && content.front() == '%';
        }

        /**
         * @brief 判断某行是否构成文档边界（列 0 的 `---` / `...` 或指令）
         * @details 三者都只在**列 0** 才有结构含义（§9.1.3、§6.8）。判定必须同时看缩进：
         *          块标量内容里缩进到内容列的 `---` / `...` / `%` 一律是内容，若只按去掉缩进
         *          后的正文判定，块内容会被当场截断。
         * @param line 待判定行
         * @return true 该行是文档边界
         */
        [[nodiscard]] static bool isDocumentBoundary(const Line &line) noexcept
        {
            return isDocumentStartLine(line) || isDocumentEndLine(line) || isDirectiveLine(line);
        }

        /**
         * @brief 扫描整个流（可含多文档）
         * @throws FormatError 文档之间缺少分隔符、文档数超限、指令非法
         */
        void scanStream()
        {
            while (true)
            {
                skipTrivia();
                if (m_lineIndex >= m_lines.size())
                {
                    return;
                }

                // 除首个文档外，文档之间必须有 `---` 或 `...` 分隔（YAML 1.2 §9.1）
                if (m_documentCount > 0 && !isDocumentBoundary(m_lines[m_lineIndex]))
                {
                    const Line &line = m_lines[m_lineIndex];
                    throw FormatError(FormatErrorKind::TrailingContent,
                                      "文档之间缺少 '---' 分隔符（YAML 1.2 §9.1）",
                                      positionOf(line, line.indent));
                }

                scanDocument();
            }
        }

        /**
         * @brief 扫描一份文档（含指令、文档头与文档尾）
         * @throws FormatError 指令非法、版本不支持或文档数超限
         */
        void scanDocument()
        {
            if (m_options.maximumDocumentCount != 0 && m_documentCount >= m_options.maximumDocumentCount)
            {
                throw FormatError(FormatErrorKind::SizeExceeded,
                                  std::format("文档份数超出上限 {}", m_options.maximumDocumentCount),
                                  currentPosition());
            }
            ++m_documentCount;

            // ① 指令段：仅允许出现在文档头之前，且必须在列 0（§6.8）
            while (m_lineIndex < m_lines.size() && isDirectiveLine(m_lines[m_lineIndex]))
            {
                parseDirective(m_lines[m_lineIndex]);
                ++m_lineIndex;
                skipTrivia();
            }

            const TextPosition documentStart = currentPosition();
            emit(YamlEvent{.type = YamlEventType::DocumentStart, .position = documentStart});

            // ② 文档头 `---`（可携带同行根节点）与紧邻的 `...`，同样只认列 0 的标记（§9.1.3）
            bool rootParsed = false;
            if (m_lineIndex < m_lines.size())
            {
                const Line             &line    = m_lines[m_lineIndex];
                const std::string_view  content = stripTrailingComment(line.content);
                if (isDocumentEndLine(line))
                {
                    // 文档头之前直接出现 `...`：本份文档为空（null）
                    emitNullScalar(NodeProperties{}, positionOf(line, line.indent));
                    ++m_lineIndex;
                    emit(YamlEvent{.type = YamlEventType::DocumentEnd, .position = positionOf(line, line.indent)});
                    return;
                }
                if (isDocumentStartLine(line))
                {
                    if (content.size() > 3)
                    {
                        // `--- value`：根节点与 `---` 同行；保持当前行下标以便续行折叠
                        const std::string_view inlineRoot   = trim(content.substr(4));
                        const std::size_t      inlineColumn = line.indent + 4 + countLeadingSpaces(content.substr(4));
                        NodeProperties         properties;
                        parseNodeFromRemainder(inlineRoot, line, inlineColumn, 0, properties, true);
                        rootParsed = true;
                    } else
                    {
                        ++m_lineIndex;
                    }
                }
            }

            // ③ 文档内容
            if (!rootParsed)
            {
                skipTrivia();
                if (m_lineIndex >= m_lines.size() || isDocumentBoundary(m_lines[m_lineIndex]))
                {
                    // 空文档按规范是 null，而不是空映射（YAML 1.2 §9.1.1）
                    emitNullScalar(NodeProperties{}, currentPosition());
                } else
                {
                    parseBlockNode(m_lines[m_lineIndex].indent, NodeProperties{});
                }
            }

            // ④ 文档尾：可选 `...`
            skipTrivia();
            if (m_lineIndex < m_lines.size())
            {
                const Line             &line    = m_lines[m_lineIndex];
                const std::string_view  content = stripTrailingComment(line.content);
                if (isDocumentEndLine(line))
                {
                    if (content.size() > 3 && !trim(content.substr(4)).empty())
                    {
                        throw FormatError(FormatErrorKind::TrailingContent,
                                          "文档结束标记 '...' 之后不允许出现内容",
                                          positionOf(line, line.indent));
                    }
                    ++m_lineIndex;
                    emit(YamlEvent{.type = YamlEventType::DocumentEnd, .position = positionOf(line, line.indent)});
                    return;
                }
            }

            emit(YamlEvent{.type = YamlEventType::DocumentEnd, .position = currentPosition()});
        }

        // ---------------------------------------------------------------- 指令

        /**
         * @brief 解析一条指令行
         * @details 指令名与参数分开处理：`%YAML` / `%TAG` 各有独立产生式（§6.8），参数必需，
         *          缺失时由各自的校验函数报错；其余名字一律按 §6.8 的保留指令
         *          `ns-reserved-directive ::= ns-directive-name ( s-separate-in-line ns-directive-parameter )*`
         *          处理——**参数可以为零个或多个**，处理器应忽略并给出警告，而不是把它当成
         *          格式错误（只有 rejectUnknownDirectives 为真时才升级为错误）。
         * @param line 指令所在行
         * @throws FormatError 指令格式非法、版本不支持或配置为拒绝未知指令
         */
        void parseDirective(const Line &line)
        {
            const std::string_view content   = stripTrailingComment(line.content);
            const TextPosition     position  = positionOf(line, line.indent);
            const std::size_t      separator = content.find_first_of(" \t");
            // 指令名到行尾为止（无参数）或到首个空白为止；参数部分只取首个空白之后的剩余文本
            const std::string_view name     = separator == std::string_view::npos ? content : content.substr(0, separator);
            const std::string_view argument = separator == std::string_view::npos ? std::string_view{} : trim(content.substr(separator + 1));

            if (name == "%YAML")
            {
                parseYamlDirective(argument, position);
                return;
            }
            if (name == "%TAG")
            {
                parseTagDirective(argument, position);
                return;
            }

            // YAML 1.2 §6.8：保留指令的参数个数不受约束，无法识别时应由处理器忽略；
            // 默认只记录警告，rejectUnknownDirectives 为真时才按配置报错
            if (m_options.rejectUnknownDirectives)
            {
                throw FormatError(FormatErrorKind::InvalidKeyword, "未知指令：" + std::string(name), position);
            }
            m_warnings.push_back("已忽略保留指令 " + std::string(name) + "（第 " + std::to_string(line.number) + " 行）");
        }

        /**
         * @brief 校验 `%YAML` 版本指令
         * @param argument 版本参数文本
         * @param position 指令位置
         * @throws FormatError 版本格式非法或版本不受支持
         */
        void parseYamlDirective(const std::string_view argument, const TextPosition &position)
        {
            const std::size_t dot = argument.find('.');
            if (dot == std::string_view::npos)
            {
                throw FormatError(FormatErrorKind::InvalidKeyword, "YAML 版本指令格式应为 '%YAML 主版本.次版本'", position);
            }

            int         major = 0;
            int         minor = 0;
            const char *begin = argument.data();
            const char *end   = argument.data() + argument.size();
            if (const auto [pointer, error] = std::from_chars(begin, begin + dot, major); error != std::errc() || pointer != begin + dot)
            {
                throw FormatError(FormatErrorKind::InvalidKeyword, "YAML 版本主版本号非法", position);
            }
            if (const auto [pointer, error] = std::from_chars(begin + dot + 1, end, minor); error != std::errc() || pointer != end)
            {
                throw FormatError(FormatErrorKind::InvalidKeyword, "YAML 版本次版本号非法", position);
            }

            // 支持 1.0~1.2；低于 1.2 给出警告，2.x 及以上直接拒绝
            if (major != 1 || minor > 2)
            {
                throw FormatError(FormatErrorKind::InvalidKeyword,
                                  std::format("不支持的 YAML 版本 {}.{}，本解析器支持 1.0 ~ 1.2", major, minor),
                                  position);
            }
            if (minor < 2)
            {
                m_warnings.push_back(std::format("YAML 版本 {}.{} 低于 1.2，标量仍按 1.2 规则解析", major, minor));
            }
        }

        /**
         * @brief 记录 `%TAG` 句柄映射
         * @param argument 形如 `!handle! prefix` 的参数
         * @param position 指令位置
         * @throws FormatError 参数格式非法
         */
        void parseTagDirective(const std::string_view argument, const TextPosition &position)
        {
            const std::size_t separator = argument.find_first_of(" \t");
            if (separator == std::string_view::npos)
            {
                throw FormatError(FormatErrorKind::InvalidKeyword, "%TAG 指令格式应为 '%TAG 句柄 前缀'", position);
            }
            const std::string_view handle = argument.substr(0, separator);
            const std::string_view prefix = trim(argument.substr(separator + 1));
            if (handle.size() < 2 || handle.front() != '!' || handle.back() != '!')
            {
                throw FormatError(FormatErrorKind::InvalidKeyword, "%TAG 句柄必须形如 '!name!'", position);
            }
            m_tagHandles[std::string(handle)] = std::string(prefix);
        }

        // ---------------------------------------------------------------- 块节点

        /**
         * @brief 解析一个块节点（映射、序列或裸标量）
         * @param indent 本块缩进
         * @param properties 节点属性
         * @throws FormatError 嵌套超限或语法非法
         */
        void parseBlockNode(const std::size_t indent, const NodeProperties &properties)
        {
            checkDepth();
            skipTrivia();
            if (m_lineIndex >= m_lines.size())
            {
                emitNullScalar(properties, currentPosition());
                return;
            }

            const Line             &line    = m_lines[m_lineIndex];
            const std::string_view  content = stripTrailingComment(line.content);

            // 块节点同样可以带锚点/标签（如整行 `&a` 或 `!!str 5`），先取出属性
            NodeProperties   combined = properties;
            std::string_view remaining;
            parseProperties(content, line, line.indent, combined, remaining);

            if (remaining.empty())
            {
                // 只有属性：节点是紧随其后的块
                ++m_lineIndex;
                parseValueBlock(indent, combined, true);
                return;
            }

            if (startsSequenceEntry(remaining))
            {
                parseBlockSequence(line.indent, combined);
                return;
            }
            if (startsExplicitKey(remaining) || findKeyValueSeparator(remaining) != std::string_view::npos)
            {
                parseBlockMapping(line.indent, combined);
                return;
            }

            // 其余情形交给行内解析：流式容器、引号/块标量与裸标量都由此覆盖
            parseInlineNode(remaining, line, line.indent + (content.size() - remaining.size()), indent, combined);
        }

        /**
         * @brief 解析块映射
         * @param indent 映射的公共缩进
         * @param properties 映射节点属性
         * @throws FormatError 缩进不一致、序列条目混入或键语法非法
         */
        void parseBlockMapping(const std::size_t indent, const NodeProperties &properties)
        {
            const DepthGuard guard(m_depth);
            checkDepth();

            emitAnchorIfNeeded(properties, currentPosition());
            YamlEvent start;
            start.type     = YamlEventType::MappingStart;
            start.tag      = properties.tag;
            start.anchor   = properties.anchor;
            start.position = currentPosition();
            emit(std::move(start));

            while (true)
            {
                skipTrivia();
                if (m_lineIndex >= m_lines.size())
                {
                    break;
                }

                const Line &line = m_lines[m_lineIndex];
                if (line.indent < indent)
                {
                    break;
                }
                if (line.indent > indent)
                {
                    throw FormatError(FormatErrorKind::UnexpectedByte,
                                      "映射内缩进不一致（YAML 1.2 §8.2.2 要求同级键对齐）",
                                      positionOf(line, line.indent));
                }

                const std::string_view content = stripTrailingComment(line.content);
                if (isDocumentBoundary(line))
                {
                    break;
                }
                if (startsSequenceEntry(content))
                {
                    throw FormatError(FormatErrorKind::UnexpectedByte,
                                      "序列条目不能与映射键同级",
                                      positionOf(line, line.indent));
                }

                if (startsExplicitKey(content))
                {
                    parseExplicitKeyEntry(indent, line);
                    continue;
                }

                const std::size_t separator = findKeyValueSeparator(content);
                if (separator == std::string_view::npos)
                {
                    throw FormatError(FormatErrorKind::UnexpectedByte,
                                      "应为 'key: value'",
                                      positionOf(line, line.indent));
                }

                const std::string_view keyText = trimRight(content.substr(0, separator));
                if (keyText.empty())
                {
                    throw FormatError(FormatErrorKind::UnexpectedByte,
                                      "映射键不能为空",
                                      positionOf(line, line.indent));
                }

                // 键节点与键行同行，就地解析（不消费行）
                parseKeyNode(keyText, line, line.indent);

                const std::size_t      remainderOffset  = separator + 1;
                const std::string_view remainder        = content.substr(remainderOffset);
                const std::string_view trimmedRemainder = trim(remainder);
                const std::size_t      remainderColumn  = line.indent + remainderOffset + countLeadingSpaces(remainder);

                NodeProperties valueProperties;
                if (trimmedRemainder.empty())
                {
                    // 值在下一行块上：消费键行后按块解析
                    ++m_lineIndex;
                    parseValueBlock(indent, valueProperties, true);
                } else
                {
                    parseNodeFromRemainder(trimmedRemainder, line, remainderColumn, indent, valueProperties, true);
                }
            }

            YamlEvent end;
            end.type     = YamlEventType::MappingEnd;
            end.position = currentPosition();
            emit(std::move(end));
        }

        /**
         * @brief 解析显式复杂键条目（`? key` 换行 `: value`，YAML 1.2 §8.2.2）
         * @details 覆盖规范给出的三种写法：`? 节点` 换行 `: 节点`、`? a` 换行 `: b`、
         *          以及与 `?` 同行的紧凑集合键 `? a: b`（后者会按 §8.2.2 的
         *          `ns-l-compact-mapping` 被改写成条目列上的块节点，因而键是集合）。
         *          键值之间允许空行、注释与块节点。
         * @param indent 映射缩进
         * @param keyLine `?` 所在行
         * @throws FormatError 语法非法
         */
        void parseExplicitKeyEntry(const std::size_t indent, const Line &keyLine)
        {
            const std::string_view content        = stripTrailingComment(keyLine.content);
            const std::string_view afterIndicator = content.substr(1);
            const std::string_view remainder      = trim(afterIndicator);
            const std::size_t      remainderColumn = keyLine.indent + 1 + countLeadingSpaces(afterIndicator);

            NodeProperties keyProperties;
            if (remainder.empty())
            {
                // 键本身是更深缩进的块节点
                ++m_lineIndex;
                parseValueBlock(indent, keyProperties, false);
            } else if (startsSequenceEntry(remainder) || startsExplicitKey(remainder) ||
                       findKeyValueSeparator(remainder) != std::string_view::npos)
            {
                // `? a: b`：键是与 `?` 同行的紧凑映射/序列，把该行改写成条目列上的块再解析，
                // 这样键会被识别成集合节点（DOM 前端按约定拒绝集合键，而不是误当成裸标量）
                const std::size_t itemColumn = keyLine.indent + 1 + countLeadingSpaces(afterIndicator);
                m_lines[m_lineIndex].content    = remainder;
                m_lines[m_lineIndex].indent     = itemColumn;
                m_lines[m_lineIndex].structural = true;
                parseBlockNode(itemColumn, keyProperties);
            } else
            {
                parseNodeFromRemainder(remainder, keyLine, remainderColumn, indent, keyProperties, false);
            }

            // 随后的 `: ` 行给出值；键值之间允许空行与注释
            skipTrivia();
            const Line      *separatorLine = nullptr;
            std::string_view valueRemainder;
            std::size_t      valueColumn = 0;

            if (m_lineIndex < m_lines.size() && m_lines[m_lineIndex].indent == indent &&
                !m_lines[m_lineIndex].content.empty() && m_lines[m_lineIndex].content.front() == ':')
            {
                separatorLine = &m_lines[m_lineIndex];
                const std::string_view afterColon = separatorLine->content.substr(1);
                valueRemainder = trim(afterColon);
                valueColumn    = separatorLine->indent + 1 + countLeadingSpaces(afterColon);
            }

            if (separatorLine == nullptr)
            {
                // 规范允许显式键没有值，此时值为 null
                emitNullScalar(NodeProperties{}, positionOf(keyLine, remainderColumn));
                return;
            }

            NodeProperties valueProperties;
            if (valueRemainder.empty())
            {
                // 值在 `:` 之后的块中：消费 `:` 行
                ++m_lineIndex;
                parseValueBlock(indent, valueProperties, true);
            } else
            {
                // 值与 `:` 同行：此处不能预先推进行号，否则行内解析会以「下一行」为基准
                // 判定续行，从而吃掉紧随其后的 `?` 条目行（parsePlainScalar 依赖该行号）
                parseNodeFromRemainder(valueRemainder, *separatorLine, valueColumn, indent, valueProperties, true);
            }
        }

        /**
         * @brief 解析一个键节点（标量键就地解析；流式容器键交由通用节点解析）
         * @param keyText 键正文
         * @param line 键所在行
         * @param column 键在行内的列偏移（0 基）
         * @throws FormatError 锚点名或标签非法
         */
        void parseKeyNode(const std::string_view keyText, const Line &line, const std::size_t column)
        {
            const TextPosition position = positionOf(line, column);
            const char           lead     = keyText.front();
            // 键始终位于当前行：解析过程可能推进行号（如流式键），此处保存以便还原，
            // 使调用方能继续在同一行上解析值
            const std::size_t savedLineIndex = m_lineIndex;

            if (lead == '[' || lead == '{')
            {
                // 复杂键：流式解析，DOM 前端会对结果做键化处理
                parseInlineNode(keyText, line, column, line.indent, NodeProperties{});
            } else if (lead == '"' || lead == '\'')
            {
                parseQuotedScalarFromView(keyText, line, column, NodeProperties{});
            } else if (lead == '&' || lead == '!')
            {
                NodeProperties   properties;
                std::string_view remaining;
                parseProperties(keyText, line, column, properties, remaining);
                if (remaining.empty())
                {
                    emitNullScalar(properties, position);
                } else
                {
                    parseInlineNode(remaining, line, column + (keyText.size() - remaining.size()), line.indent, properties);
                }
            } else
            {
                emitScalar(std::string(keyText), YamlScalarStyle::Plain, NodeProperties{}, position);
            }

            m_lineIndex = savedLineIndex;
        }

        /**
         * @brief 解析块序列
         * @param indent 序列的公共缩进
         * @param properties 序列节点属性
         * @throws FormatError 缩进不一致
         */
        void parseBlockSequence(const std::size_t indent, const NodeProperties &properties)
        {
            const DepthGuard guard(m_depth);
            checkDepth();

            emitAnchorIfNeeded(properties, currentPosition());
            YamlEvent start;
            start.type     = YamlEventType::SequenceStart;
            start.tag      = properties.tag;
            start.anchor   = properties.anchor;
            start.position = currentPosition();
            emit(std::move(start));

            while (true)
            {
                skipTrivia();
                if (m_lineIndex >= m_lines.size())
                {
                    break;
                }

                const Line &line = m_lines[m_lineIndex];
                if (line.indent < indent)
                {
                    break;
                }

                const std::string_view content = stripTrailingComment(line.content);
                if (!startsSequenceEntry(content))
                {
                    if (line.indent > indent)
                    {
                        throw FormatError(FormatErrorKind::UnexpectedByte,
                                          "序列内缩进不一致",
                                          positionOf(line, line.indent));
                    }
                    break;
                }

                // 条目正文：`-` 之后的内容及其在行内的列偏移
                const std::string_view afterDash     = content.substr(1);
                const std::size_t      leadingSpaces = countLeadingSpaces(afterDash);
                const std::string_view itemText      = trim(afterDash);
                const std::size_t      itemColumn    = line.indent + 1 + leadingSpaces;

                if (itemText.empty())
                {
                    // 条目值为下一行的块（同缩进的 `-` 是兄弟条目，故不允许零缩进序列）
                    ++m_lineIndex;
                    parseValueBlock(indent, NodeProperties{}, false);
                    continue;
                }

                // 「- key: value」与「- - 1」：把该行改写为缩进到条目列的子块再解析
                if (startsSequenceEntry(itemText) || startsExplicitKey(itemText) ||
                    findKeyValueSeparator(itemText) != std::string_view::npos)
                {
                    m_lines[m_lineIndex].content    = itemText;
                    m_lines[m_lineIndex].indent     = itemColumn;
                    m_lines[m_lineIndex].structural = true;
                    parseBlockNode(itemColumn, NodeProperties{});
                    continue;
                }

                // 条目的块上下文缩进取序列自身缩进：块标量内容只需深于序列缩进
                // （`- |` 换行 `  text` 依赖这一点），裸标量续行判定同样以它为基准
                const Line virtualLine{line.raw, itemText, indent, line.number, line.offset, true};
                parseNodeFromRemainder(itemText, virtualLine, itemColumn, indent, NodeProperties{}, false);
            }

            YamlEvent end;
            end.type     = YamlEventType::SequenceEnd;
            end.position = currentPosition();
            emit(std::move(end));
        }

        /**
         * @brief 解析键值对中「值在下一行块」的情形
         * @param parentIndent 键所在缩进
         * @param properties 值节点属性
         * @param allowZeroIndentSequence 是否允许零缩进序列（`ports:` 换行 `- 80`，§8.2.1）
         */
        void parseValueBlock(const std::size_t parentIndent, const NodeProperties &properties, const bool allowZeroIndentSequence)
        {
            skipTrivia();
            if (m_lineIndex >= m_lines.size())
            {
                emitNullScalar(properties, currentPosition());
                return;
            }

            const Line &line = m_lines[m_lineIndex];
            if (line.indent > parentIndent)
            {
                parseBlockNode(line.indent, properties);
                return;
            }

            // 零缩进序列：值块与键同缩进且以 `-` 开头
            if (allowZeroIndentSequence && line.indent == parentIndent &&
                startsSequenceEntry(stripTrailingComment(line.content)))
            {
                parseBlockSequence(line.indent, properties);
                return;
            }

            emitNullScalar(properties, positionOf(line, line.indent));
        }

        /**
         * @brief 从某段正文解析节点（先取属性，再视情况解析同行节点或下一行块）
         * @param remainder 正文（已去掉键与分隔符）
         * @param line 正文所在行
         * @param column 正文在行内的列偏移（0 基）
         * @param parentIndent 块上下文缩进
         * @param properties 已有属性（通常为空）
         * @param allowZeroIndentSequence 属性之后若转块解析，是否允许零缩进序列
         */
        void parseNodeFromRemainder(std::string_view remainder, const Line &line, const std::size_t column,
                                    const std::size_t parentIndent, NodeProperties properties,
                                    const bool allowZeroIndentSequence)
        {
            std::string_view remaining;
            parseProperties(remainder, line, column, properties, remaining);

            if (remaining.empty())
            {
                // 只有属性（如 `key: &a` 或 `- !!map`）：节点是紧随其后的块
                ++m_lineIndex;
                parseValueBlock(parentIndent, properties, allowZeroIndentSequence);
                return;
            }

            parseInlineNode(remaining, line, column + (remainder.size() - remaining.size()), parentIndent, properties);
        }

        /**
         * @brief 解析节点属性（锚点与标签），允许任意顺序
         * @param text 输入正文
         * @param line 所在行
         * @param column 正文起始列（0 基）
         * @param properties 输入输出参数，累积解析到的属性
         * @param remaining 输出参数，去掉属性后的剩余正文
         * @throws FormatError 锚点名或标签格式非法
         */
        void parseProperties(std::string_view text, const Line &line, const std::size_t column,
                             NodeProperties &properties, std::string_view &remaining)
        {
            std::size_t cursor = 0;
            while (cursor < text.size())
            {
                const char character = text[cursor];
                if (character != '&' && character != '!')
                {
                    break;
                }

                const std::size_t tokenStart = cursor;
                while (cursor < text.size() && text[cursor] != ' ' && text[cursor] != '\t')
                {
                    ++cursor;
                }
                const std::string_view token = text.substr(tokenStart, cursor - tokenStart);

                if (character == '&')
                {
                    if (token.size() < 2)
                    {
                        throw FormatError(FormatErrorKind::InvalidKeyword, "锚点名不能为空", positionOf(line, column + tokenStart));
                    }
                    properties.anchor = std::string(token.substr(1));
                } else
                {
                    properties.tag = expandTag(token, positionOf(line, column + tokenStart));
                }

                while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t'))
                {
                    ++cursor;
                }
            }

            remaining = text.substr(cursor);
        }

        /**
         * @brief 展开标签写法为完整标签
         * @details 支持 `!!suffix`（核心 schema）、`!handle!suffix`（%TAG 句柄）、
         *          `!<verbatim>`、`!local` 与裸 `!`。
         * @param token 标签原文
         * @param position 标签位置
         * @return std::string 展开后的标签
         * @throws FormatError 标签格式非法
         */
        std::string expandTag(const std::string_view token, const TextPosition &position)
        {
            if (token.size() <= 1)
            {
                return std::string(token);
            }

            if (token[1] == '<')
            {
                if (token.back() != '>')
                {
                    throw FormatError(FormatErrorKind::InvalidKeyword, "标签格式非法：" + std::string(token), position);
                }
                return std::string(token.substr(2, token.size() - 3));
            }

            // `!!suffix` 与 `!handle!suffix` 都命中第二个 '!'，统一走句柄表
            const std::size_t secondBang = token.find('!', 1);
            if (secondBang != std::string_view::npos)
            {
                const std::string_view handle = token.substr(0, secondBang + 1);
                const std::string_view suffix = token.substr(secondBang + 1);
                const auto             iterator = m_tagHandles.find(std::string(handle));
                if (iterator == m_tagHandles.end())
                {
                    m_warnings.push_back("未声明的标签句柄 " + std::string(handle) + "，标签按原样保留");
                    return std::string(token);
                }

                std::string expanded = iterator->second;
                // 后缀中的 %XX 按 URI 转义还原（§6.8.2）
                for (std::size_t index = 0; index < suffix.size(); ++index)
                {
                    if (suffix[index] == '%' && index + 2 < suffix.size())
                    {
                        const int high = hexDigitValue(suffix[index + 1]);
                        const int low  = hexDigitValue(suffix[index + 2]);
                        if (high >= 0 && low >= 0)
                        {
                            expanded.push_back(static_cast<char>(high * 16 + low));
                            index += 2;
                            continue;
                        }
                    }
                    expanded.push_back(suffix[index]);
                }
                return expanded;
            }

            // `!local` 本地标签保持原样
            return std::string(token);
        }

        // ---------------------------------------------------------------- 行内节点

        /**
         * @brief 解析行内节点（流式容器、引号标量、块标量、别名或裸标量）
         * @param text 正文
         * @param line 所在行
         * @param column 列偏移（0 基）
         * @param blockIndent 父块缩进，供裸标量跨行折叠判定续行是否更深（§7.3.3）
         * @param properties 节点属性
         * @throws FormatError 语法非法
         */
        void parseInlineNode(const std::string_view text, const Line &line, const std::size_t column,
                             const std::size_t blockIndent, const NodeProperties &properties)
        {
            if (text.empty())
            {
                emitNullScalar(properties, positionOf(line, column));
                return;
            }

            const TextPosition position = positionOf(line, column);

            switch (text.front())
            {
                case '[':
                    parseFlowCollection(text, line, column, true, properties);
                    return;
                case '{':
                    parseFlowCollection(text, line, column, false, properties);
                    return;
                case '"':
                case '\'':
                    parseQuotedScalarFromView(text, line, column, properties);
                    return;
                case '|':
                case '>':
                    parseBlockScalar(text, line, column, properties);
                    return;
                case '*':
                    parseAlias(text, position);
                    ++m_lineIndex;
                    return;
                default:
                    break;
            }

            parsePlainScalar(text, line, column, blockIndent, properties);
        }

        /**
         * @brief 解析别名引用 `*name`
         * @param text 以 `*` 开头的正文
         * @param position 位置
         * @throws FormatError 别名为空或之后有多余内容
         */
        void parseAlias(const std::string_view text, const TextPosition &position)
        {
            const std::size_t      end  = text.find_first_of(" \t");
            const std::string_view name = end == std::string_view::npos ? text.substr(1) : text.substr(1, end - 1);
            if (name.empty())
            {
                throw FormatError(FormatErrorKind::InvalidKeyword, "别名不能为空", position);
            }
            if (end != std::string_view::npos && !trim(text.substr(end)).empty())
            {
                throw FormatError(FormatErrorKind::TrailingContent, "别名之后不允许出现内容", position);
            }

            YamlEvent aliasEvent;
            aliasEvent.type     = YamlEventType::Alias;
            aliasEvent.alias    = std::string(name);
            aliasEvent.position = position;
            emit(std::move(aliasEvent));
        }

        /**
         * @brief 解析裸标量（含跨行折叠，YAML 1.2 §7.3.3）
         * @param firstText 首行正文
         * @param line 首行
         * @param column 列偏移（0 基，仅用于错误定位）
         * @param blockIndent 父块缩进，续行必须严格深于它才会并入标量
         * @param properties 节点属性
         */
        void parsePlainScalar(const std::string_view firstText, const Line &line, const std::size_t column,
                              const std::size_t blockIndent, const NodeProperties &properties)
        {
            const TextPosition   position  = positionOf(line, column);
            const std::string_view firstLine = stripTrailingComment(firstText);

            // 单行裸标量占绝大多数：直接构造，不做任何折叠中间缓冲
            if (m_lineIndex + 1 >= m_lines.size())
            {
                emitScalar(std::string(trimRight(firstLine)), YamlScalarStyle::Plain, properties, position);
                ++m_lineIndex;
                return;
            }

            std::string       folded(trimRight(firstLine));
            std::size_t       emptyRun  = 0;
            std::size_t       nextIndex = m_lineIndex + 1;
            bool              multiLine = false;

            while (nextIndex < m_lines.size())
            {
                const Line &candidate = m_lines[nextIndex];
                if (!candidate.structural)
                {
                    if (isBlankLine(candidate.raw))
                    {
                        // 空行参与折叠：每个空行贡献一个换行（§7.3.3）
                        ++emptyRun;
                        ++nextIndex;
                        multiLine = true;
                        continue;
                    }
                    // 整行注释终止裸标量
                    break;
                }
                // 续行必须严格深于父块缩进：列 0 的文档标记（§9.1.3）与指令（§6.8）缩进必然
                // 不大于父块缩进，已被这一条排除，无需再单独判边界
                if (candidate.indent <= blockIndent)
                {
                    break;
                }

                const std::string_view candidateContent = stripTrailingComment(candidate.content);
                if (candidateContent.empty() || startsSequenceEntry(candidateContent) ||
                    startsExplicitKey(candidateContent) ||
                    findKeyValueSeparator(candidateContent) != std::string_view::npos)
                {
                    break;
                }

                // 更深的续行：单换行折叠为空格，空行保留换行
                if (emptyRun == 0)
                {
                    folded.push_back(' ');
                } else
                {
                    folded.append(emptyRun, kLineBreak);
                }
                folded += trimRight(candidateContent);
                emptyRun  = 0;
                nextIndex = nextIndex + 1;
                multiLine = true;
            }

            if (!multiLine)
            {
                emitScalar(std::move(folded), YamlScalarStyle::Plain, properties, position);
                ++m_lineIndex;
                return;
            }

            // 尾随空行属于标量之后的内容，按 §7.3.3 被剥离，不计入标量文本
            m_lineIndex = nextIndex;
            emitScalar(std::move(folded), YamlScalarStyle::Plain, properties, position);
        }

        // ---------------------------------------------------------------- 引号标量

        /**
         * @brief 从视图解析引号标量（可能跨行）
         * @param text 以引号开头的正文
         * @param line 起始行
         * @param column 列偏移（0 基）
         * @param properties 节点属性
         * @throws FormatError 未闭合或转义非法
         */
        void parseQuotedScalarFromView(const std::string_view text, const Line &line, const std::size_t column,
                                       const NodeProperties &properties)
        {
            const char           quote    = text.front();
            const TextPosition position = positionOf(line, column);

            // 先尝试单行闭合：绝大多数引号标量不跨行，可零折叠直接解码
            const std::size_t closing = findClosingQuote(text, 0, quote);
            if (closing != std::string_view::npos)
            {
                const std::string_view body = text.substr(1, closing - 1);
                if (quote == '\'')
                {
                    emitScalar(decodeSingleQuotedSingleLine(body), YamlScalarStyle::SingleQuoted, properties, position);
                } else
                {
                    emitScalar(decodeDoubleQuoted(body, position), YamlScalarStyle::DoubleQuoted, properties, position);
                }
                ++m_lineIndex;
                return;
            }

            // 跨行：拼接后续原始行，再在拼接结果上寻找收尾引号并折叠
            std::string joined;
            joined.reserve(text.size() * 2);
            joined += text.substr(1);
            std::size_t currentIndex = m_lineIndex + 1;
            bool        closed       = false;

            while (currentIndex < m_lines.size())
            {
                joined.push_back(kLineBreak);
                joined += m_lines[currentIndex].raw;
                ++currentIndex;
                if (findClosingQuote(joined, 0, quote) != std::string_view::npos)
                {
                    closed = true;
                    break;
                }
            }

            if (!closed)
            {
                throw FormatError(FormatErrorKind::UnterminatedString,
                                  quote == '"' ? "双引号标量未闭合" : "单引号标量未闭合",
                                  position);
            }

            const std::size_t      closeIndex = findClosingQuote(joined, 0, quote);
            const std::string_view rawBody    = std::string_view(joined).substr(0, closeIndex);
            if (quote == '\'')
            {
                emitScalar(foldAndDecodeSingleQuoted(rawBody), YamlScalarStyle::SingleQuoted, properties, position);
            } else
            {
                emitScalar(foldAndDecodeDoubleQuoted(rawBody, position), YamlScalarStyle::DoubleQuoted, properties, position);
            }

            m_lineIndex = currentIndex;
        }

        /**
         * @brief 定位引号标量的收尾引号
         * @param text 起始于开场引号的文本（可能含换行）
         * @param openingIndex 开场引号下标
         * @param quote 引号字符
         * @return std::size_t 收尾引号下标，未找到返回 npos
         */
        [[nodiscard]] static std::size_t findClosingQuote(const std::string_view text, const std::size_t openingIndex, const char quote) noexcept
        {
            for (std::size_t index = openingIndex + 1; index < text.size(); ++index)
            {
                const char character = text[index];
                if (quote == '\'')
                {
                    if (character != '\'')
                    {
                        continue;
                    }
                    // 单引号内连续两个引号表示一个字面引号
                    if (index + 1 < text.size() && text[index + 1] == '\'')
                    {
                        ++index;
                        continue;
                    }
                    return index;
                }

                if (character == '\\')
                {
                    ++index;
                    continue;
                }
                if (character == '"')
                {
                    return index;
                }
            }
            return std::string_view::npos;
        }

        /**
         * @brief 解码单引号标量正文（`''` 还原为 `'`）
         * @param body 引号内正文
         * @return std::string 解码结果
         */
        [[nodiscard]] static std::string decodeSingleQuotedSingleLine(const std::string_view body)
        {
            if (body.find("''") == std::string_view::npos)
            {
                return std::string(body);
            }

            std::string result;
            result.reserve(body.size());
            for (std::size_t index = 0; index < body.size(); ++index)
            {
                if (body[index] == '\'' && index + 1 < body.size() && body[index + 1] == '\'')
                {
                    result.push_back('\'');
                    ++index;
                    continue;
                }
                result.push_back(body[index]);
            }
            return result;
        }

        /**
         * @brief 折叠并解码跨行单引号标量
         * @param body 引号内正文（含换行）
         * @return std::string 折叠并解码后的文本
         */
        [[nodiscard]] static std::string foldAndDecodeSingleQuoted(const std::string_view body)
        {
            return decodeSingleQuotedSingleLine(foldQuotedBreaks(body, false));
        }

        /**
         * @brief 折叠并解码跨行双引号标量
         * @param body 引号内正文（含换行）
         * @param position 起始位置
         * @return std::string 折叠并解码后的文本
         * @throws FormatError 转义非法
         */
        [[nodiscard]] static std::string foldAndDecodeDoubleQuoted(const std::string_view body, const TextPosition &position)
        {
            return decodeDoubleQuoted(foldQuotedBreaks(body, true), position);
        }

        /**
         * @brief 折叠引号标量内的换行（YAML 1.2 §7.3.1）
         * @details 单个换行折叠为空格；k 个连续空行折叠为 k 个换行；行尾与续行首空白被剥离。
         *          双引号下反斜杠引导的换行是转义换行，原样保留交由转义解码处理。
         * @param body 引号内正文
         * @param isDoubleQuoted 是否为双引号
         * @return std::string 折叠后的文本
         */
        [[nodiscard]] static std::string foldQuotedBreaks(const std::string_view body, const bool isDoubleQuoted)
        {
            std::string result;
            result.reserve(body.size());

            std::size_t index = 0;
            while (index < body.size())
            {
                const char character = body[index];
                if (character != '\n')
                {
                    if (isDoubleQuoted && character == '\\' && index + 1 < body.size() && body[index + 1] == '\n')
                    {
                        // 转义换行：保留标记，由解码器吃掉整段并跳过续行空白
                        result.push_back('\\');
                        result.push_back('\n');
                        index += 2;
                        continue;
                    }
                    result.push_back(character);
                    ++index;
                    continue;
                }

                // 换行：先剥离行尾空白，再统计随后的空行数量
                while (!result.empty() && (result.back() == ' ' || result.back() == '\t'))
                {
                    result.pop_back();
                }

                std::size_t next       = index + 1;
                std::size_t emptyLines = 0;
                while (next < body.size())
                {
                    const std::size_t lineEnd  = body.find('\n', next);
                    const std::size_t lineStop = lineEnd == std::string_view::npos ? body.size() : lineEnd;
                    if (trim(body.substr(next, lineStop - next)).empty())
                    {
                        ++emptyLines;
                        next = lineEnd == std::string_view::npos ? body.size() : lineEnd + 1;
                        continue;
                    }
                    break;
                }

                if (emptyLines == 0)
                {
                    result.push_back(' ');
                } else
                {
                    result.append(emptyLines, kLineBreak);
                }

                while (next < body.size() && (body[next] == ' ' || body[next] == '\t'))
                {
                    ++next;
                }
                index = next;
            }

            return result;
        }

        /**
         * @brief 解码双引号标量正文（YAML 1.2 §5.7 的完整转义表）
         * @param body 引号内正文
         * @param position 起始位置（错误定位）
         * @return std::string 解码结果
         * @throws FormatError 转义序列非法
         */
        [[nodiscard]] static std::string decodeDoubleQuoted(const std::string_view body, const TextPosition &position)
        {
            std::string result;
            result.reserve(body.size());

            for (std::size_t index = 0; index < body.size(); ++index)
            {
                const char character = body[index];
                if (character != '\\')
                {
                    result.push_back(character);
                    continue;
                }

                if (index + 1 >= body.size())
                {
                    throw FormatError(FormatErrorKind::InvalidEscape, "双引号标量以孤立的反斜杠结尾", position);
                }

                const char escape = body[++index];
                switch (escape)
                {
                    case '0':
                        result.push_back('\0');
                        break;
                    case 'a':
                        result.push_back('\x07');
                        break;
                    case 'b':
                        result.push_back('\b');
                        break;
                    case 't':
                    case '\t':
                        result.push_back('\t');
                        break;
                    case 'n':
                        result.push_back('\n');
                        break;
                    case 'v':
                        result.push_back('\v');
                        break;
                    case 'f':
                        result.push_back('\f');
                        break;
                    case 'r':
                        result.push_back('\r');
                        break;
                    case 'e':
                        result.push_back('\x1B');
                        break;
                    case '"':
                        result.push_back('"');
                        break;
                    case '/':
                        result.push_back('/');
                        break;
                    case '\\':
                        result.push_back('\\');
                        break;
                    case ' ':
                        result.push_back(' ');
                        break;
                    case 'N':
                        TextEscapes::appendUtf8(result, 0x85); // U+0085 下一行
                        break;
                    case '_':
                        TextEscapes::appendUtf8(result, 0xA0); // U+00A0 不换行空格
                        break;
                    case 'L':
                        TextEscapes::appendUtf8(result, 0x2028); // U+2028 行分隔符
                        break;
                    case 'P':
                        TextEscapes::appendUtf8(result, 0x2029); // U+2029 段分隔符
                        break;
                    case 'x':
                    case 'u':
                    case 'U':
                    {
                        // 定长十六进制转义；\u 需处理 UTF-16 代理对（§5.7）
                        const std::size_t digitCount = escape == 'x' ? 2 : (escape == 'u' ? 4 : 8);
                        std::size_t       cursor     = index + 1;
                        std::uint32_t     codePoint  = 0;
                        try
                        {
                            codePoint = TextEscapes::decodeHex(body, cursor, digitCount, position);
                        } catch (const FormatError &inner)
                        {
                            throw FormatError(FormatErrorKind::InvalidEscape, "十六进制转义非法：" + inner.reason(), position);
                        }

                        if (escape == 'u' && codePoint >= 0xD800 && codePoint <= 0xDBFF)
                        {
                            if (cursor + 1 < body.size() && body[cursor] == '\\' && body[cursor + 1] == 'u')
                            {
                                std::size_t         lowCursor = cursor + 2;
                                const std::uint32_t low       = TextEscapes::decodeHex(body, lowCursor, 4, position);
                                if (low >= 0xDC00 && low <= 0xDFFF)
                                {
                                    codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                                    cursor    = lowCursor;
                                } else
                                {
                                    throw FormatError(FormatErrorKind::SurrogatePairError, "代理项配对非法：低代理项不在 DC00~DFFF", position);
                                }
                            } else
                            {
                                throw FormatError(FormatErrorKind::SurrogatePairError, "孤立的 UTF-16 高代理项", position);
                            }
                        } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF)
                        {
                            throw FormatError(FormatErrorKind::SurrogatePairError, "孤立的 UTF-16 低代理项", position);
                        }

                        TextEscapes::appendUtf8(result, codePoint);
                        index = cursor - 1;
                        break;
                    }
                    case '\n':
                        // 转义换行：只起续行作用，不产生字符，并跳过续行前导空白
                        while (index + 1 < body.size() && (body[index + 1] == ' ' || body[index + 1] == '\t'))
                        {
                            ++index;
                        }
                        break;
                    default:
                        throw FormatError(FormatErrorKind::InvalidEscape,
                                          std::format("未知的转义序列 '\\{}'", escape),
                                          position);
                }
            }

            return result;
        }

        // ---------------------------------------------------------------- 块标量

        /**
         * @brief 解析块标量 `|` 与 `>`（YAML 1.2 §8.1）
         * @param text 以指示符开头的正文
         * @param line 指示符所在行
         * @param column 列偏移（0 基）
         * @param properties 节点属性
         * @throws FormatError 头部非法
         */
        void parseBlockScalar(const std::string_view text, const Line &line, const std::size_t column,
                              const NodeProperties &properties)
        {
            const TextPosition    position = positionOf(line, column);
            const BlockScalarHeader header   = parseBlockScalarHeader(text, position);

            // §8.1.1.1：内容缩进 = 父节点缩进 n + 缩进指示 m；未显式给出 m 时由首个
            // 非空内容行自动探测，且该缩进必须大于 n（因为 m >= 1）。序列条目里的
            // `- |` 会把本行改写为条目内容列，此处取到的仍是序列自身缩进，正好是 n。
            const std::size_t parentIndent = line.indent;

            std::vector<std::string_view> contentLines;
            std::vector<bool>             moreIndented;
            std::size_t                   contentIndent       = parentIndent + header.explicitIndent;
            bool                          indentationResolved = header.explicitIndent != 0;

            // 末行换行是否存在：输入末尾没有换行时最后一行不产生换行（§8.1.1.2 中
            // l-chomped-last 允许「文件结束」，此时 clip/keep 都无换行可保留）
            bool        trailingLineBreakExists = false;
            std::size_t nextIndex               = m_lineIndex + 1;

            if (!indentationResolved)
            {
                // 自动探测只看首个非空行；前导空行不参与定缩进（§8.1.1.1），
                // 但它们仍属于块内容（§8.1.2 的 l-empty 位于内容产生式内部）。
                // 探针行必须比父缩进更深，因此它必然不在列 0，不可能是文档边界（§9.1.3）
                std::size_t probe = nextIndex;
                while (probe < m_lines.size() && isBlankLine(m_lines[probe].raw))
                {
                    ++probe;
                }
                if (probe < m_lines.size() && m_lines[probe].indent > parentIndent)
                {
                    contentIndent       = m_lines[probe].indent;
                    indentationResolved = true;
                }
            }

            while (nextIndex < m_lines.size())
            {
                const Line &candidate = m_lines[nextIndex];
                if (isBlankLine(candidate.raw))
                {
                    // 空行是内容的一部分：折叠时换算成换行，chomping 时计入尾部空行数
                    contentLines.push_back({});
                    moreIndented.push_back(false);
                } else if (!indentationResolved || candidate.indent < contentIndent)
                {
                    // 缩进不足（或始终没能探测出缩进）：块内容到此结束。
                    // §9.1.3 的文档标记与 §6.8 的指令只在列 0 成立，其缩进必然小于内容缩进，
                    // 因此这一条缩进判定已经覆盖「`---` / `...` / 指令行结束块标量」的情形
                    break;
                } else
                {
                    // 块标量内部没有注释、指令、转义与文档标记语法：达到内容缩进的行一律
                    // 原样保留，`#`、`---`、`%YAML` 开头的行都必须是内容
                    // （§8.1.2 l-nb-literal-text / l-nb-folded-text）
                    contentLines.push_back(candidate.raw.substr(contentIndent));
                    moreIndented.push_back(candidate.indent > contentIndent);
                }

                trailingLineBreakExists = isLineTerminated(nextIndex);
                ++nextIndex;
            }

            m_lineIndex = nextIndex;
            emitScalar(buildBlockScalarText(contentLines, moreIndented, header, trailingLineBreakExists),
                       header.isFolded ? YamlScalarStyle::Folded : YamlScalarStyle::Literal,
                       properties, position);
        }

        /**
         * @brief 判断某行在输入中是否以换行结束
         * @param index 行下标
         * @return true 该行之后存在换行符（CRLF 同样算）
         */
        [[nodiscard]] bool isLineTerminated(const std::size_t index) const noexcept
        {
            const Line &line = m_lines[index];
            return line.offset + line.raw.size() < m_input.size();
        }

        /**
         * @brief 解析块标量头部（指示符、chomping 与显式缩进位数）
         * @param text 以 `|` 或 `>` 开头的正文
         * @param position 位置
         * @return BlockScalarHeader 头部解析结果
         * @throws FormatError 指示符重复或字符非法
         */
        [[nodiscard]] static BlockScalarHeader parseBlockScalarHeader(const std::string_view text, const TextPosition &position)
        {
            BlockScalarHeader header;
            header.isFolded = text.front() == '>';

            std::size_t cursor       = 1;
            bool        chompingSeen = false;
            bool        indentSeen   = false;

            while (cursor < text.size())
            {
                const char character = text[cursor];
                if (character == ' ' || character == '\t')
                {
                    ++cursor;
                    continue;
                }
                if (character == '#')
                {
                    break; // 头部之后是注释
                }
                if (character == '-' || character == '+')
                {
                    if (chompingSeen)
                    {
                        throw FormatError(FormatErrorKind::InvalidKeyword, "块标量头部出现重复的 chomping 指示符", position);
                    }
                    header.chomping = character == '-' ? ChompingMode::Strip : ChompingMode::Keep;
                    chompingSeen    = true;
                    ++cursor;
                    continue;
                }
                if (character >= '1' && character <= '9')
                {
                    if (indentSeen)
                    {
                        throw FormatError(FormatErrorKind::InvalidKeyword, "块标量头部出现重复的缩进指示符", position);
                    }
                    header.explicitIndent = static_cast<std::size_t>(character - '0');
                    indentSeen            = true;
                    ++cursor;
                    continue;
                }

                throw FormatError(FormatErrorKind::InvalidKeyword,
                                  std::format("块标量头部出现非法字符 '{}'", character),
                                  position);
            }

            return header;
        }

        /**
         * @brief 依据折叠与 chomping 规则拼装块标量文本（§8.1.1.2 / §8.1.3）
         * @param contentLines 内容行（已去掉内容缩进）
         * @param moreIndented 各行是否比内容缩进更深
         * @param header 头部指示
         * @param trailingLineBreakExists 内容末行在输入中是否真的有换行（末尾截断时为 false）
         * @return std::string 最终文本
         */
        [[nodiscard]] static std::string buildBlockScalarText(const std::vector<std::string_view> &contentLines,
                                                              const std::vector<bool> &moreIndented,
                                                              const BlockScalarHeader &header,
                                                              const bool trailingLineBreakExists)
        {
            // 尾部空行按 chomping 处理：strip/clip 丢弃，keep 每个空行换算成一个换行
            std::size_t trimmedCount = contentLines.size();
            while (trimmedCount > 0 && contentLines[trimmedCount - 1].empty())
            {
                --trimmedCount;
            }
            const std::size_t trailingEmptyCount = contentLines.size() - trimmedCount;
            // 「末行换行」只属于真正写出过文本的块；整块只有空行时没有这一份换行（§8.1.1.2）
            const bool        hasTextContent     = trimmedCount > 0;

            std::string body;
            std::size_t emptyRun             = 0;
            bool        previousMoreIndented = false;
            bool        emittedText          = false;

            for (std::size_t index = 0; index < trimmedCount; ++index)
            {
                const std::string_view content = contentLines[index];
                if (content.empty())
                {
                    ++emptyRun; // 空行先攒着，由下一个文本行按折叠规则换算成换行
                    continue;
                }

                if (!emittedText)
                {
                    // 首个文本行：它之前的空行各自贡献一个换行（§8.1.2 l-empty）
                    body.append(emptyRun, kLineBreak);
                } else if (header.isFolded && !moreIndented[index] && !previousMoreIndented)
                {
                    if (emptyRun == 0)
                    {
                        body.push_back(' '); // §8.1.3：单个换行折叠为空格
                    } else
                    {
                        body.append(emptyRun, kLineBreak); // 空行原样保留为换行，不再折叠
                    }
                } else
                {
                    // 字面风格，或至少一侧是 more-indented（§8.1.3 规定这类行不参与折叠）
                    body.append(emptyRun + 1, kLineBreak);
                }

                body += content;
                emptyRun             = 0;
                previousMoreIndented = moreIndented[index];
                emittedText          = true;
            }

            std::size_t trailingBreaks = 0;
            switch (header.chomping)
            {
                case ChompingMode::Strip:
                    // strip：末行换行与尾部空行全部去掉
                    trailingBreaks = 0;
                    break;
                case ChompingMode::Clip:
                    // clip：只保留末行换行，尾部空行去掉
                    trailingBreaks = (hasTextContent && trailingLineBreakExists) ? 1 : 0;
                    break;
                case ChompingMode::Keep:
                    // keep：末行换行与尾部空行全部保留
                    trailingBreaks = trailingEmptyCount + (hasTextContent ? 1 : 0);
                    break;
            }

            // 输入末尾没有换行时，保留统计里那个「本不存在的换行」要去掉（§8.1.1.2）
            if (!trailingLineBreakExists && trailingBreaks > 0)
            {
                --trailingBreaks;
            }

            body.append(trailingBreaks, kLineBreak);
            return body;
        }

        // ---------------------------------------------------------------- 流式容器

        /**
         * @brief 解析流式容器（可跨行，含行内注释）
         * @param text 以 `[` 或 `{` 开头的正文
         * @param line 起始行
         * @param column 列偏移（0 基）
         * @param isSequence 是否为序列
         * @param properties 节点属性
         * @throws FormatError 容器未闭合或流式语法非法
         */
        void parseFlowCollection(const std::string_view text, const Line &line, const std::size_t column,
                                 const bool isSequence, const NodeProperties &properties)
        {
            const DepthGuard guard(m_depth);
            checkDepth();

            std::size_t       endOffset    = 0;
            std::size_t       endLineIndex = 0;
            const std::string flowText     = collectFlowText(line.offset + column, endOffset, endLineIndex);
            static_cast<void>(text);
            static_cast<void>(endOffset);

            const TextPosition position = positionOf(line, column);
            emitAnchorIfNeeded(properties, position);

            YamlEvent start;
            start.type     = isSequence ? YamlEventType::SequenceStart : YamlEventType::MappingStart;
            start.tag      = properties.tag;
            start.anchor   = properties.anchor;
            start.position = position;
            emit(std::move(start));

            FlowCursor cursor;
            cursor.text   = flowText;
            cursor.index  = 0;
            cursor.line   = line.number;
            cursor.column = line.indent + column;

            if (isSequence)
            {
                parseFlowSequenceBody(cursor, position);
            } else
            {
                parseFlowMappingBody(cursor, position);
            }

            YamlEvent end;
            end.type     = isSequence ? YamlEventType::SequenceEnd : YamlEventType::MappingEnd;
            end.position = position;
            emit(std::move(end));

            m_lineIndex = endLineIndex;
        }

        /**
         * @brief 收集流式容器的完整文本（跨行折叠为空格，注释剔除）
         * @param startOffset 起始字节偏移（指向 `[` 或 `{`）
         * @param endOffset 输出参数，闭括号之后一个字节的偏移
         * @param endLineIndex 输出参数，容器结束后第一个未消费行的下标
         * @return std::string 归一化后的流式文本
         * @throws FormatError 容器未闭合
         */
        [[nodiscard]] std::string collectFlowText(const std::size_t startOffset, std::size_t &endOffset, std::size_t &endLineIndex)
        {
            std::string result;
            result.reserve(64);

            std::size_t offset = startOffset;
            int         depth  = 0;
            char        quote  = '\0';

            while (offset < m_input.size())
            {
                const char character = m_input[offset];

                if (quote != '\0')
                {
                    result.push_back(character);
                    if (quote == '\'' && character == '\'' && offset + 1 < m_input.size() && m_input[offset + 1] == '\'')
                    {
                        result.push_back('\'');
                        offset += 2;
                        continue;
                    }
                    if (quote == '"' && character == '\\' && offset + 1 < m_input.size())
                    {
                        result.push_back(m_input[offset + 1]);
                        offset += 2;
                        continue;
                    }
                    if (character == quote)
                    {
                        quote = '\0';
                    }
                    ++offset;
                    continue;
                }

                if (character == '"' || character == '\'')
                {
                    quote = character;
                    result.push_back(character);
                    ++offset;
                    continue;
                }

                if (character == '#')
                {
                    // 流式容器内的注释：`#` 前置空白或位于行首时跳到行尾
                    const bool isComment = offset == startOffset || m_input[offset - 1] == ' ' ||
                                           m_input[offset - 1] == '\t' || m_input[offset - 1] == '\n';
                    if (isComment)
                    {
                        while (offset < m_input.size() && m_input[offset] != '\n')
                        {
                            ++offset;
                        }
                        continue;
                    }
                    result.push_back(character);
                    ++offset;
                    continue;
                }

                if (character == '\n')
                {
                    // 跨行折叠为空格；行尾空白一并去掉
                    while (!result.empty() && (result.back() == ' ' || result.back() == '\t'))
                    {
                        result.pop_back();
                    }
                    result.push_back(' ');
                    ++offset;
                    continue;
                }

                if (character == '[' || character == '{')
                {
                    ++depth;
                } else if (character == ']' || character == '}')
                {
                    --depth;
                    if (depth == 0)
                    {
                        result.push_back(character);
                        endOffset    = offset + 1;
                        // 消费掉容器结束所在行
                        endLineIndex = lineIndexForOffset(offset) + 1;
                        return result;
                    }
                }

                result.push_back(character);
                ++offset;
            }

            throw FormatError(FormatErrorKind::UnterminatedContainer,
                              m_input[startOffset] == '[' ? "流式序列未闭合，应为 ']'" : "流式映射未闭合，应为 '}'",
                              TextPosition{1, 1, startOffset});
        }

        /**
         * @brief 由字节偏移定位行下标
         * @param offset 字节偏移
         * @return std::size_t 行下标（偏移越界时返回最后一行下标）
         */
        [[nodiscard]] std::size_t lineIndexForOffset(const std::size_t offset) const noexcept
        {
            std::size_t low  = 0;
            std::size_t high = m_lines.size();
            while (low < high)
            {
                const std::size_t middle = low + (high - low) / 2;
                if (m_lines[middle].offset <= offset)
                {
                    low = middle + 1;
                } else
                {
                    high = middle;
                }
            }
            return low == 0 ? 0 : low - 1;
        }

        /**
         * @brief 跳过流式文本中的空白
         * @param cursor 游标
         */
        static void skipFlowSpaces(FlowCursor &cursor) noexcept
        {
            while (cursor.index < cursor.text.size() &&
                   (cursor.text[cursor.index] == ' ' || cursor.text[cursor.index] == '\t'))
            {
                ++cursor.index;
            }
        }

        /**
         * @brief 解析流式序列主体（游标置于 `[` 处）
         * @param cursor 游标
         * @param position 容器位置
         * @throws FormatError 语法非法
         */
        void parseFlowSequenceBody(FlowCursor &cursor, const TextPosition &position)
        {
            ++cursor.index; // 跳过 '['
            while (true)
            {
                skipFlowSpaces(cursor);
                if (cursor.index >= cursor.text.size())
                {
                    throw FormatError(FormatErrorKind::UnterminatedContainer, "流式序列未闭合，应为 ']'", position);
                }
                if (cursor.text[cursor.index] == ']')
                {
                    ++cursor.index;
                    return;
                }

                parseFlowNode(cursor);
                skipFlowSpaces(cursor);

                if (cursor.index < cursor.text.size() && cursor.text[cursor.index] == ',')
                {
                    ++cursor.index;
                    continue;
                }
                if (cursor.index < cursor.text.size() && cursor.text[cursor.index] == ']')
                {
                    ++cursor.index;
                    return;
                }
                throw FormatError(FormatErrorKind::UnexpectedByte, "流式序列条目之间应为 ',' 或 ']'", position);
            }
        }

        /**
         * @brief 解析流式映射主体（游标置于 `{` 处）
         * @param cursor 游标
         * @param position 容器位置
         * @throws FormatError 语法非法
         */
        void parseFlowMappingBody(FlowCursor &cursor, const TextPosition &position)
        {
            ++cursor.index; // 跳过 '{'
            while (true)
            {
                skipFlowSpaces(cursor);
                if (cursor.index >= cursor.text.size())
                {
                    throw FormatError(FormatErrorKind::UnterminatedContainer, "流式映射未闭合，应为 '}'", position);
                }
                if (cursor.text[cursor.index] == '}')
                {
                    ++cursor.index;
                    return;
                }

                // 键
                parseFlowNode(cursor);
                skipFlowSpaces(cursor);
                if (cursor.index >= cursor.text.size() || cursor.text[cursor.index] != ':')
                {
                    throw FormatError(FormatErrorKind::UnexpectedByte, "流式映射条目必须写成 key: value", position);
                }
                ++cursor.index; // 消费 ':'
                parseFlowNode(cursor);

                skipFlowSpaces(cursor);
                if (cursor.index < cursor.text.size() && cursor.text[cursor.index] == ',')
                {
                    ++cursor.index;
                    continue;
                }
                if (cursor.index < cursor.text.size() && cursor.text[cursor.index] == '}')
                {
                    ++cursor.index;
                    return;
                }
                throw FormatError(FormatErrorKind::UnexpectedByte, "流式映射条目之间应为 ',' 或 '}'", position);
            }
        }

        /**
         * @brief 解析流式容器内的一个节点（含锚点/标签属性）
         * @param cursor 游标
         * @throws FormatError 语法非法
         */
        void parseFlowNode(FlowCursor &cursor)
        {
            checkDepth();
            skipFlowSpaces(cursor);

            const TextPosition position{cursor.line, cursor.column + cursor.index + 1, 0};

            // 空条目（如 `[a, , b]`）按 null 处理
            if (cursor.index >= cursor.text.size() ||
                cursor.text[cursor.index] == ',' || cursor.text[cursor.index] == ']' || cursor.text[cursor.index] == '}')
            {
                emitNullScalar(NodeProperties{}, position);
                return;
            }

            NodeProperties properties;
            while (cursor.index < cursor.text.size() &&
                   (cursor.text[cursor.index] == '&' || cursor.text[cursor.index] == '!'))
            {
                const std::size_t tokenStart = cursor.index;
                while (cursor.index < cursor.text.size() &&
                       cursor.text[cursor.index] != ' ' && cursor.text[cursor.index] != '\t' &&
                       cursor.text[cursor.index] != ',' && cursor.text[cursor.index] != ']' && cursor.text[cursor.index] != '}')
                {
                    ++cursor.index;
                }
                const std::string_view token = cursor.text.substr(tokenStart, cursor.index - tokenStart);
                if (token.front() == '&')
                {
                    properties.anchor = std::string(token.substr(1));
                } else
                {
                    properties.tag = expandTag(token, position);
                }
                skipFlowSpaces(cursor);
            }

            if (cursor.index >= cursor.text.size())
            {
                emitNullScalar(properties, position);
                return;
            }

            const char lead = cursor.text[cursor.index];
            if (lead == '[' || lead == '{')
            {
                const DepthGuard guard(m_depth);
                checkDepth();
                const bool isSequence = lead == '[';
                emitAnchorIfNeeded(properties, position);

                YamlEvent start;
                start.type     = isSequence ? YamlEventType::SequenceStart : YamlEventType::MappingStart;
                start.tag      = properties.tag;
                start.anchor   = properties.anchor;
                start.position = position;
                emit(std::move(start));

                if (isSequence)
                {
                    parseFlowSequenceBody(cursor, position);
                } else
                {
                    parseFlowMappingBody(cursor, position);
                }

                YamlEvent end;
                end.type     = isSequence ? YamlEventType::SequenceEnd : YamlEventType::MappingEnd;
                end.position = position;
                emit(std::move(end));
                return;
            }

            if (lead == '"' || lead == '\'')
            {
                const char        quote   = lead;
                const std::size_t closing = findClosingQuote(cursor.text, cursor.index, quote);
                if (closing == std::string_view::npos)
                {
                    throw FormatError(FormatErrorKind::UnterminatedString, "流式容器内引号标量未闭合", position);
                }
                const std::string_view body = cursor.text.substr(cursor.index + 1, closing - cursor.index - 1);
                cursor.index = closing + 1;
                if (quote == '\'')
                {
                    emitScalar(decodeSingleQuotedSingleLine(body), YamlScalarStyle::SingleQuoted, properties, position);
                } else
                {
                    emitScalar(decodeDoubleQuoted(body, position), YamlScalarStyle::DoubleQuoted, properties, position);
                }
                return;
            }

            if (lead == '*')
            {
                const std::size_t start = cursor.index;
                while (cursor.index < cursor.text.size() &&
                       cursor.text[cursor.index] != ',' && cursor.text[cursor.index] != ']' && cursor.text[cursor.index] != '}')
                {
                    ++cursor.index;
                }
                parseAlias(trim(cursor.text.substr(start, cursor.index - start)), position);
                return;
            }

            // 裸标量：直到 `,` `]` `}` 或 `: `（后接空白/结尾/分隔符）为止
            const std::size_t scalarStart = cursor.index;
            while (cursor.index < cursor.text.size())
            {
                const char character = cursor.text[cursor.index];
                if (character == ',' || character == ']' || character == '}')
                {
                    break;
                }
                if (character == ':' &&
                    (cursor.index + 1 >= cursor.text.size() || cursor.text[cursor.index + 1] == ' ' ||
                     cursor.text[cursor.index + 1] == ',' || cursor.text[cursor.index + 1] == ']' ||
                     cursor.text[cursor.index + 1] == '}'))
                {
                    break;
                }
                ++cursor.index;
            }

            emitScalar(std::string(trim(cursor.text.substr(scalarStart, cursor.index - scalarStart))),
                       YamlScalarStyle::Plain, properties, position);
        }

        /**
         * @brief 定位映射键值分隔冒号
         * @details 跳过引号与流式容器；只有「冒号 + 空白/行尾」才是分隔符（YAML 1.2 §7.4.2）
         * @param content 行正文
         * @return std::size_t 分隔符下标，未找到返回 npos
         */
        [[nodiscard]] static std::size_t findKeyValueSeparator(const std::string_view content) noexcept
        {
            for (std::size_t index = 0; index < content.size(); ++index)
            {
                const char character = content[index];

                if (character == '"' || character == '\'')
                {
                    const std::size_t closing = findClosingQuote(content, index, character);
                    if (closing == std::string_view::npos)
                    {
                        return std::string_view::npos;
                    }
                    index = closing;
                    continue;
                }
                if (character == '[' || character == '{')
                {
                    // 行首的流式容器不是映射，其内部冒号也不构成键值分隔
                    return std::string_view::npos;
                }
                if (character == '#')
                {
                    return std::string_view::npos;
                }

                if (character == ':' &&
                    (index + 1 == content.size() || content[index + 1] == ' ' || content[index + 1] == '\t'))
                {
                    return index;
                }
            }
            return std::string_view::npos;
        }

    };

    // ---------------------------------------------------------------- YamlReader 转发

    YamlReader::YamlReader() :
        m_implementation(std::make_unique<YamlReaderImplementation>())
    {
    }

    YamlReader::YamlReader(const std::string_view text, const YamlParseOptions &options) :
        m_implementation(std::make_unique<YamlReaderImplementation>())
    {
        // 零拷贝：仅记录视图，不复制整份输入
        m_implementation->m_input    = text;
        m_implementation->m_options  = options;
        m_implementation->m_finished = true;
    }

    YamlReader::~YamlReader() = default;

    YamlReader::YamlReader(YamlReader &&other) noexcept = default;

    YamlReader &YamlReader::operator=(YamlReader &&other) noexcept = default;

    void YamlReader::feed(const std::string_view chunk)
    {
        if (m_implementation->m_consuming)
        {
            throw FormatError(FormatErrorKind::UnexpectedByte,
                              "已开始取事件，不能再向读取器喂入输入",
                              TextPosition{});
        }
        if (m_implementation->m_finished)
        {
            throw FormatError(FormatErrorKind::UnexpectedByte,
                              "已声明输入结束，不能再向读取器喂入输入",
                              TextPosition{});
        }

        // 首次 feed 起转入自有缓冲模式；此后视图指向该缓冲
        m_implementation->m_ownedInput.append(chunk);
        m_implementation->m_input = m_implementation->m_ownedInput;
    }

    void YamlReader::finish()
    {
        if (m_implementation->m_finished)
        {
            return;
        }
        m_implementation->m_finished = true;
        // 自有缓冲模式下重新固定视图，确保后续扫描读到完整输入
        if (!m_implementation->m_ownedInput.empty())
        {
            m_implementation->m_input = m_implementation->m_ownedInput;
        }
    }

    void YamlReader::ensureScanned()
    {
        // push 模式下若调用方忘记 finish，这里补一次，避免静默得到空事件流
        if (!m_implementation->m_finished)
        {
            finish();
        }
        // 一旦开始扫描就进入「取用中」状态，此后禁止再 feed
        m_implementation->m_consuming = true;
        m_implementation->run();
    }

    bool YamlReader::hasNext()
    {
        ensureScanned();
        return m_implementation->m_eventIndex < m_implementation->m_events.size();
    }

    std::optional<YamlEvent> YamlReader::nextEvent()
    {
        ensureScanned();
        m_implementation->m_consuming = true;
        if (m_implementation->m_eventIndex >= m_implementation->m_events.size())
        {
            return std::nullopt;
        }
        // 事件只被取用一次，直接移动出缓存，避免字符串二次拷贝
        return std::optional<YamlEvent>(std::move(m_implementation->m_events[m_implementation->m_eventIndex++]));
    }

    std::vector<YamlEvent> YamlReader::readAll(const std::string_view text, const YamlParseOptions &options)
    {
        YamlReader           reader(text, options);
        std::vector<YamlEvent> events;
        while (reader.hasNext())
        {
            if (std::optional<YamlEvent> event = reader.nextEvent())
            {
                events.push_back(std::move(*event));
            }
        }
        return events;
    }

    const YamlParseOptions &YamlReader::options() const noexcept
    {
        return m_implementation->m_options;
    }

    std::vector<std::string> &YamlReader::warnings()
    {
        // 即便尚未取事件也可能已产生警告，这里确保扫描已执行
        ensureScanned();
        return m_implementation->m_warnings;
    }
} // namespace AsynGyanis::Base
