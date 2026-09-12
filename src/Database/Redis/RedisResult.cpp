#include "Database/Redis/RedisResult.h"

#ifdef DATABASE_HAS_REDIS
// hiredis 头只在实现文件里包含，前置声明见 RedisConnection.h 的全局作用域
#include <hiredis/hiredis.h>
#endif

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace AsynGyanis::Database
{
    namespace
    {
        // Redis 的回复元素没有名字，列名只能按下标合成（value0、value1…）；
        // 用 constexpr 常量取代宏，改名或换前缀只需动这一处
        constexpr const char *kSyntheticColumnNamePrefix = "value";
    } // namespace

#ifdef DATABASE_HAS_REDIS

    namespace
    {
        /**
         * @brief 按长度摘取回复节点携带的原始文本
         * @param sourceReply 回复节点，可为 nullptr
         * @return std::string 文本副本；节点为空或没有文本域时返回空串
         */
        std::string copyReplyText(const redisReply *sourceReply)
        {
            // hiredis 用 str + len 表达文本，不保证零终止且可以内嵌 '\0'，必须按长度拷贝
            if (sourceReply == nullptr || sourceReply->str == nullptr)
            {
                return {};
            }
            return std::string(sourceReply->str, sourceReply->len);
        }

        /**
         * @brief 把无法用 std::string 直接表达的回复节点压成一段可读文本
         * @param sourceReply 回复节点，可为 nullptr
         * @return std::string 十进制数字、"[a, b]" 形式的嵌套串或原始文本
         */
        std::string flattenReplyText(const redisReply *sourceReply)
        {
            if (sourceReply == nullptr)
            {
                return {};
            }

            switch (sourceReply->type)
            {
                // 列表的备选类型只有 std::vector<std::string>：整数转成十进制文本，
                // 至少保住数值信息（旧实现把非字符串子元素直接丢掉，整数数组会返回空列表）
                case REDIS_REPLY_INTEGER:
                    return std::to_string(sourceReply->integer);

                case REDIS_REPLY_ARRAY:
                {
                    // 更深一层的数组同样无法在 DatabaseValue 里表达结构，
                    // 这里串成 "[a, b]" 形式：牺牲层级，换取「不静默丢数据」；
                    // 需要精确结构时用 nativeHandle() 自行遍历
                    std::string nestedText("[");
                    for (size_t index = 0; index < sourceReply->elements; ++index)
                    {
                        if (index > 0)
                        {
                            nestedText += ", ";
                        }
                        nestedText += flattenReplyText(sourceReply->element[index]);
                    }
                    nestedText += ']';
                    return nestedText;
                }

                // nil 子元素用空串占位，保证下标对齐（例如 MGET 未命中的那一项）
                case REDIS_REPLY_NIL:
                    return {};

                // STRING / STATUS / ERROR 走文本；RESP3 的 DOUBLE、VERB 在 hiredis 里
                // 也保留服务端原始文本（DOUBLE 的文本恰是它的十进制表示），
                // 因此统一取文本即可，无需引用较新 hiredis 才有的枚举常量
                default:
                    return copyReplyText(sourceReply);
            }
        }
    } // namespace

    RedisResult::RedisResult(redisReply *const ownedReply) : m_replyPointer(ownedReply)
    {
        // 空回复代表「什么都没有」：类型与列数保持默认值，isEmpty() 自然为 true，
        // 析构也不会对 nullptr 调用 freeReplyObject
        if (m_replyPointer == nullptr)
        {
            return;
        }

        m_replyType = m_replyPointer->type;

        // 列数等于元素个数：数组回复取 elements；nil 既无值也无元素，按「0 列」的空集处理；
        // 其余标量回复（整数、批量字符串、状态、error）都是一行一列
        if (m_replyType == REDIS_REPLY_ARRAY)
        {
            m_columnCount = m_replyPointer->elements;
        }
        else if (m_replyType != REDIS_REPLY_NIL)
        {
            m_columnCount = 1;
        }

        // 构造属于非 const 写路径：在这里把 error 回复的原文摘进 m_lastError，
        // 之后所有 const 读取接口都只读不写（旧实现在 getValue() 里写它，既违反基类契约也编译不过）
        if (m_replyType == REDIS_REPLY_ERROR)
        {
            m_lastError = copyReplyText(m_replyPointer);
        }
    }

    RedisResult::~RedisResult()
    {
        // 回复由结果集独占所有权：freeReplyObject 会递归释放全部子元素与各自的文本缓冲，
        // 不需要（也不应该）再自己遍历释放。该接口没有返回码，析构路径也无从向调用方报错，
        // 因此这里不做额外检查
        if (m_replyPointer != nullptr)
        {
            freeReplyObject(m_replyPointer);
            m_replyPointer = nullptr;
        }
    }

    DatabaseValue RedisResult::getValue(const size_t index) const
    {
        // 取值失败一律回 monostate：本方法是 const 读取路径，按基类契约绝不改写 m_lastError；
        // 索引越界的判定用缓存的列数，避免把 size_t 下标直接交给只接受无符号计数的 hiredis 字段
        if (m_replyPointer == nullptr || index >= m_columnCount)
        {
            return std::monostate{};
        }

        // 数组回复：第 index 列就是第 index 个元素
        if (m_replyType == REDIS_REPLY_ARRAY)
        {
            return convertReply(m_replyPointer->element[index]);
        }

        // 标量回复只有一列（上面已保证 index == 0），整份回复即该列的值
        return convertReply(m_replyPointer);
    }

    bool RedisResult::isError() const
    {
        return m_replyType == REDIS_REPLY_ERROR;
    }

    DatabaseValue RedisResult::convertReply(const redisReply *const sourceReply) const
    {
        if (sourceReply == nullptr)
        {
            return std::monostate{};
        }

        switch (sourceReply->type)
        {
            // 批量字符串、状态回复与 error 回复都由 str/len 承载，按长度拷贝成 string：
            // 二进制安全，内嵌 '\0' 不丢，也不依赖零终止符
            case REDIS_REPLY_STRING:
            case REDIS_REPLY_STATUS:
            case REDIS_REPLY_ERROR:
                return copyReplyText(sourceReply);

            // hiredis 把 Redis 的 64 位有符号整数放在 integer 里，与 DatabaseValue 的整型备选完全对齐
            case REDIS_REPLY_INTEGER:
                return static_cast<std::int64_t>(sourceReply->integer);

            // nil 与「没有值」在 DatabaseValue 里统一用 monostate 表达
            case REDIS_REPLY_NIL:
                return std::monostate{};

            case REDIS_REPLY_ARRAY:
            {
                // 顶层数组映射成列表；子元素全部压成文本，这是列表备选类型只有字符串造成的有损映射，
                // 取舍与理由见 flattenReplyText()
                std::vector<std::string> elements;
                elements.reserve(sourceReply->elements);
                for (size_t index = 0; index < sourceReply->elements; ++index)
                {
                    elements.push_back(flattenReplyText(sourceReply->element[index]));
                }
                return elements;
            }

            default:
            {
                // RESP3 扩展类型（DOUBLE / MAP / SET / ATTR / PUSH / VERB）：本驱动从不发送 HELLO 3，
                // 正常路径走不到这里。真遇到时优先按原始文本取回（DOUBLE、VERB 有文本），
                // 既保住信息，也让本文件不依赖各版本命名不一致的枚举常量；纯容器型只能回 monostate
                return (sourceReply->str != nullptr) ? DatabaseValue{ copyReplyText(sourceReply) } : DatabaseValue{ std::monostate{} };
            }
        }
    }

#else // DATABASE_HAS_REDIS —— 未编译 hiredis：结果集退化成永远为空的只读对象

    // 桩构建里不可能有回复，构造函数刻意不接收参数值（也就无需 freeReplyObject），
    // 全部状态保持默认：0 行 0 列、isEmpty() 为 true
    RedisResult::RedisResult(redisReply *)
    {
    }

    RedisResult::~RedisResult()
    {
        // 桩构建里没有 redisReply，也就没有 freeReplyObject 要做的事；
        // 虚析构只能在类内默认化，因此这里给出函数体而不是类外的 = default
    }

    DatabaseValue RedisResult::getValue(const size_t) const
    {
        return std::monostate{};
    }

    bool RedisResult::isError() const
    {
        return false;
    }

    // 仅为满足头文件里的声明而保留：桩构建里没有可转换的回复节点
    DatabaseValue RedisResult::convertReply(const redisReply *) const
    {
        return std::monostate{};
    }

#endif // DATABASE_HAS_REDIS

    // ------------------------------------------------------------------------
    // 以下逻辑只依赖构造阶段快照下来的成员，不触碰 hiredis，
    // 因此真实实现与桩实现共用同一份定义（桩下 m_columnCount 恒为 0，一切自然退化为空集）
    // ------------------------------------------------------------------------

    bool RedisResult::next()
    {
        // 空结果集没有那一行可交；旧实现先自增下标再比较，游标语义与 rowCount() 互相矛盾
        if (m_columnCount == 0)
        {
            return false;
        }

        // 唯一一行已经交出过：后续调用恒为 false，于是 while (result->next()) 恰好只走一轮
        if (m_hasReturnedRow)
        {
            return false;
        }

        // 单向标志：只有 reset() 会把它复位，读取路径不改状态
        m_hasReturnedRow = true;
        return true;
    }

    size_t RedisResult::rowCount() const
    {
        // 退化单行契约：有列就有那一行，否则为 0；不存在 SQLite 那种「行数未知」的第三种情形
        return m_columnCount > 0 ? 1 : 0;
    }

    size_t RedisResult::columnCount() const
    {
        return m_columnCount;
    }

    std::optional<std::string> RedisResult::columnName(const size_t index) const
    {
        // Redis 无列名概念，越界（含空结果集）一律空值，与基类契约一致
        if (index >= m_columnCount)
        {
            return std::nullopt;
        }

        // 合成名字按下标生成，本身不承载语义
        return std::string(kSyntheticColumnNamePrefix) + std::to_string(index);
    }

    std::optional<size_t> RedisResult::columnIndex(const std::string_view name) const
    {
        // 顺序扫描并复用 columnName()，让名字生成规则只有一个真值来源；
        // 列数是个位数级别的规模，这里为每次比较生成临时串换来的可读写得比省下一次分配更值
        for (size_t index = 0; index < m_columnCount; ++index)
        {
            if (const std::optional<std::string> synthesizedName = columnName(index);
                synthesizedName.has_value() && name == synthesizedName.value())
            {
                return index;
            }
        }

        return std::nullopt;
    }

    DatabaseValue RedisResult::getValue(const std::string_view name) const
    {
        // 先按名解析索引再走索引重载，保证两条路径的越界与 nil 判定完全一致
        if (const std::optional<size_t> index = columnIndex(name); index.has_value())
        {
            return getValue(*index);
        }

        return std::monostate{};
    }

    std::vector<std::string> RedisResult::columnNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_columnCount);
        for (size_t index = 0; index < m_columnCount; ++index)
        {
            // columnName() 在本循环的界内不会返回空值，仍用空串兜底以维持「长度 == columnCount()」
            names.push_back(columnName(index).value_or(std::string{}));
        }
        return names;
    }

    void RedisResult::reset()
    {
        // 只复位游标标志：数据全在内存里，回复本身既不释放也不重建
        m_hasReturnedRow = false;
    }

    bool RedisResult::isEmpty() const
    {
        // 与 rowCount() == 0 严格等价：0 列即没有那一行
        return m_columnCount == 0;
    }

} // namespace AsynGyanis::Database
