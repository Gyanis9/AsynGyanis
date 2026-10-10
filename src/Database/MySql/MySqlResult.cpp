// min / max 函数式宏会破坏 std::numeric_limits<T>::max() 等写法，必须在任何头之前挡住它们，
// 理由与写法说明见 MySqlConnection.cpp 同一位置的中文注释
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Database/MySql/MySqlResult.h"

#ifdef DATABASE_HAS_MYSQL

// MySQL C API 头只在本实现文件里包含，前置声明见 MySqlConnection.h 的全局作用域。
// 两种发行布局（顶层 mysql.h / mysql/ 子目录 mysql.h）的兼容写法必须与 MySqlConnection.cpp 保持一致，
// 说明见该文件同一位置的中文注释
#if __has_include(<mysql/mysql.h>)
#include <mysql/mysql.h>
#else
#include <mysql.h>
#endif

// 列值到 DatabaseValue 的类型映射与参数化执行路径共用一份实现，保证两条协议路径取值语义一致
#include "Database/MySql/MySqlValueConversion.h"

#endif // DATABASE_HAS_MYSQL

#include <format>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace AsynGyanis::Database
{
#ifdef DATABASE_HAS_MYSQL

    MySqlResult::MySqlResult(MYSQL_RES *const ownedResult, const std::int64_t affectedRowCount, const std::uint64_t generatedInsertId) :
        m_result(ownedResult), m_affectedRowCount(affectedRowCount)
    {
        // 自增标识要交出去的宽度是有符号 64 位，而 BIGINT UNSIGNED 的自增列可以从 2^63 起播种：
        // 那种值按补码转换会变成一个看着合理的负数，比报不出来危险得多，因此如实留 0 并写明原因
        if (generatedInsertId > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        {
            m_lastError = std::format("MySQL 自增标识 {} 超出有符号 64 位上界，本接口无法如实表达（按 0 交出）；"
                                      "请改从该列本身查询取值",
                                      generatedInsertId);
        } else
        {
            m_lastInsertRowId = static_cast<std::int64_t>(generatedInsertId);
        }

        // 空句柄即「写操作的成功回执」：没有列也没有行，两个计数保持默认 0，isEmpty() 因此恒为 true
        if (m_result == nullptr)
        {
            return;
        }

        // 数据已由 mysql_store_result 完整读进客户端内存，这两个计数此后不会再变，
        // 构造时一次快照，之后所有 const 接口都只读缓存（预读成功的结果不会给出「行数未知」的哨兵值）
        m_rowCount    = static_cast<size_t>(mysql_num_rows(m_result));
        m_columnCount = static_cast<size_t>(mysql_num_fields(m_result));
    }

    MySqlResult::MySqlResult(MYSQL_RES *const ownedResult, MYSQL *const connectionHandle, const Detail::StreamingTag) noexcept :
        m_result(ownedResult), m_connectionHandle(connectionHandle), m_isStreaming(true)
    {
        // 列元数据在流式结果上是即时可用的（回复的头一段就是列定义），所以列数照常快照；
        // 行数**不快照**：mysql_num_rows 在取完全部行之前给的是 0，这里把它留成 0 并在 rowCount() 的
        // 文档里说明「0 是未知」，比等到读完再更新诚实——本类也不假装能提供随机定位
        if (m_result != nullptr)
        {
            m_columnCount = static_cast<size_t>(mysql_num_fields(m_result));
        }
    }

    std::unique_ptr<MySqlResult> MySqlResult::forStreaming(MYSQL_RES *const ownedResult, MYSQL *const connectionHandle)
    {
        // 刻意不用 make_unique：那个 new 表达式发生在标准库的函数体里，拿不到本类的私有构造访问权。
        // 失败路径的所有权仍在调用方手上（它带着 ResultReleaser 守卫，构造抛出时由守卫释放），这里不多释放一次
        return std::unique_ptr<MySqlResult>(new MySqlResult(ownedResult, connectionHandle, Detail::StreamingTag{}));
    }

    MySqlResult::~MySqlResult()
    {
        // 结果集由本对象独占所有权：mysql_free_result 一次性回收行缓冲与列元数据。
        // 该接口没有返回码，析构路径也无从向调用方报错，因此这里不做额外检查。
        // m_currentRow 指向的正是这份内部缓冲，随之一起失效，绝不能再单独解引用
        if (m_result != nullptr)
        {
            mysql_free_result(m_result);
            m_result = nullptr;
        }
    }

    bool MySqlResult::next()
    {
        // 写回执没有游标，永远「没有下一行」；顺手把游标置空，保证 getValue() 的判定与之一致
        if (m_result == nullptr)
        {
            m_currentRow = nullptr;
            return false;
        }

        // 预读结果集的行都在客户端内存里，返回空指针只可能是已到末尾，不会再有「读取出错」这一分支。
        // 按基类契约本方法属只读路径，不改写 m_lastError
        m_currentRow = mysql_fetch_row(m_result);
        if (m_currentRow != nullptr)
        {
            return true;
        }

        if (!m_isStreaming)
        {
            return false;
        }

        // 流式的空指针有两解：回复流真的读完了，或者中途断了。二者对调用方是完全不同的事——后者意味着
        // 它拿到的是一份**残缺**结果，按「读完了」收尾就是把截断当成完整。判据是连接上的错误状态：
        // 读完时 mysql_errno(连接) 为 0，断了则带着服务端/客户端的错误码
        const unsigned int errorNumber = m_connectionHandle != nullptr ? mysql_errno(m_connectionHandle) : 0U;
        if (errorNumber == 0U)
        {
            return false;
        }

        const char *const nativeText = mysql_error(m_connectionHandle);
        m_lastError                  = std::format("MySQL 流式读取中断（错误码 {}{}）：剩下的行取不到了，"
                                                   "已读到的部分不构成完整结果集。这不是「读完了」，"
                                                   "请按需要重查或改用 execute() 的预读路径",
                                                   errorNumber, nativeText != nullptr && nativeText[0] != '\0' ? std::format("，{}", nativeText) : std::string{});
        return false;
    }

    std::optional<std::string> MySqlResult::columnName(const size_t index) const
    {
        // 先用缓存的无符号列数判界：越界索引直接强转成 unsigned int 形参会回绕成另一个合法列号
        if (m_result == nullptr || index >= m_columnCount)
        {
            return std::nullopt;
        }

        // 元数据数组由 MYSQL_RES 自己持有，生命周期到 mysql_free_result 为止；
        // 这里立刻拷成 std::string 交出去，不把内部指针泄漏给调用方
        const MYSQL_FIELD *currentField = mysql_fetch_field_direct(m_result, static_cast<unsigned int>(index));
        if (currentField == nullptr || currentField->name == nullptr)
        {
            return std::nullopt;
        }

        return std::string(currentField->name);
    }

    std::optional<size_t> MySqlResult::columnIndex(const std::string_view name) const
    {
        if (m_result == nullptr || name.empty())
        {
            return std::nullopt;
        }

        // mysql_fetch_fields 一次给出整张元数据数组（长度即列数）；
        // 无列或元数据读取失败时它返回空指针，直接 fields[i] 解引用会崩，这里必须先判空
        const MYSQL_FIELD *fields = mysql_fetch_fields(m_result);
        if (fields == nullptr)
        {
            return std::nullopt;
        }

        for (size_t index = 0; index < m_columnCount; ++index)
        {
            const char *rawName = fields[index].name;
            // 表达式列可能没有名字，跳过而不是当成匹配，避免把空名误认成一次命中
            if (rawName == nullptr)
            {
                continue;
            }

            // 显式构造 std::string_view 而不是依赖 char* 的隐式转换：比较语义写明白，也确保是逐字节比较。
            // 元数据里的列名是客户端库另建的一份 C 字符串（与行值不同，它保证零终止），因此可以只给首地址。
            // 列标识符的大小写敏感性由服务端排序规则决定，本方法按原文精确匹配（区分大小写）；
            // 同名列先到先得，与 MySQL 自身按名取列的规则一致
            if (name == std::string_view(rawName))
            {
                return index;
            }
        }

        return std::nullopt;
    }

    DatabaseValue MySqlResult::getValue(const size_t index) const
    {
        // 游标没停在有效行上（未 next()、已走完、刚 reset()）时行指针与长度表都不可信，
        // 不做这层清理会读出上一行的残值；这里按「无值」返回，与读到 NULL 列的表现一致
        if (m_result == nullptr || m_currentRow == nullptr || index >= m_columnCount)
        {
            return std::monostate{};
        }

        const char *rawValue = m_currentRow[index];
        // 列值为 SQL NULL：与「空串」「0」是三件不同的事，只有 NULL 才映射成 monostate
        if (rawValue == nullptr)
        {
            return std::monostate{};
        }

        // 长度表与行指针同批产出，只到下一次 mysql_fetch_row 之前有效，因此必须在推进游标前取。
        // 它是唯一能正确界定 TEXT/BLOB 边界的依据：行缓冲里字段首尾相接，不保证每个都以 '\0' 结束
        const unsigned long *columnLengths = mysql_fetch_lengths(m_result);
        if (columnLengths == nullptr)
        {
            // 拿不到长度就没有可信的取值边界，宁缺毋滥：按 C 字符串猜边界会截断二进制数据
            return std::monostate{};
        }

        return convertValue(rawValue, static_cast<size_t>(columnLengths[index]), index);
    }

    void MySqlResult::reset()
    {
        // 写回执本来就是空集，reset 是安全的空操作
        if (m_result == nullptr)
        {
            return;
        }

        // 先清掉上一轮的错误：本函数代表一次新的尝试，不能让历史文本冒充本次结果
        m_lastError.clear();

        // 流式结果没有可退回的位置：行已经从服务端流过来就收不回去，官方的随机定位也只对预读结果有效。
        // 这里不假装能重扫（游标原地不动、继续 next() 仍回 false），并把原因留给调用方
        if (m_isStreaming)
        {
            m_currentRow = nullptr;
            m_lastError  = "流式结果集不支持重扫：行是一次性流过来的，收不回也定位不了；"
                           "要重扫请把数据读进自己的容器，或改用 execute() 的预读路径";
            return;
        }

        // 预读结果集才支持随机定位（本驱动一律用 mysql_store_result，所以恒满足该前提）。
        // mysql_data_seek 是 void 接口，失败也无从知晓，这是它与 sqlite3_reset 的差别
        mysql_data_seek(m_result, 0);

        // 游标退回首行之前，当前行随之失效：不清这个指针会让 getValue() 继续读上一行的缓冲
        m_currentRow = nullptr;
    }

    DatabaseValue MySqlResult::convertValue(const char *const rawValue, const size_t byteLength, const size_t index) const
    {
        // 类型信息只能来自列元数据；取不到（索引判界已在调用方做过）时唯一安全的映射是按文本交出字节，
        // 至少不丢数据，也比猜一个类型更可靠
        const MYSQL_FIELD *currentField = (m_result != nullptr && index < m_columnCount) ? mysql_fetch_field_direct(m_result, static_cast<unsigned int>(index)) : nullptr;
        if (currentField == nullptr)
        {
            return std::string(rawValue, byteLength);
        }

        // 列类型到 DatabaseValue 的映射与参数化执行路径共用 Detail::convertColumnText 一份实现：
        // 列类型以 int 传递是为了不在该共享头的签名里暴露第三方枚举；字符集号是必需的，
        // 因为 BLOB 与 TEXT 在协议层共用同一个类型码，只有字符集能区分二者
        return Detail::convertColumnText(static_cast<int>(currentField->type), static_cast<unsigned int>(currentField->charsetnr), rawValue, byteLength);
    }

#else // DATABASE_HAS_MYSQL —— 桩实现：没有客户端库，结果集退化成永远为空的只读对象

    // 桩构建里不可能有 MYSQL_RES，构造函数刻意不使用参数值（也就无需 mysql_free_result），
    // 全部状态保持默认：0 行 0 列、isEmpty() 为 true。影响行数同样按默认 0 处理——
    // 桩下 connect() 必失败，任何写语句都执行不了，报出非零行数只会是假信息
    MySqlResult::MySqlResult(MYSQL_RES *, const std::int64_t, const std::uint64_t)
    {
    }

    // 桩构建里既没有 MYSQL_RES 也没有连接句柄：流式构造退化成与预读构造同一份「永远为空」的对象，
    // 因此 forStreaming() 给出的仍是可用的空结果集（0 行 0 列、next() 恒 false），调用方在桩下
    // 本来就取不到数据，判据不必为流式另开一条
    MySqlResult::MySqlResult(MYSQL_RES *, MYSQL *, const Detail::StreamingTag) noexcept
    {
    }

    std::unique_ptr<MySqlResult> MySqlResult::forStreaming(MYSQL_RES *, MYSQL *)
    {
        return std::make_unique<MySqlResult>(nullptr);
    }

    MySqlResult::~MySqlResult()
    {
        // 桩构建里没有 MYSQL_RES，也就没有 mysql_free_result 要做的事；
        // 虚析构只能在类内首次声明处 = default，类外定义必须给出函数体而不是再写 = default
    }

    bool MySqlResult::next()
    {
        return false;
    }

    std::optional<std::string> MySqlResult::columnName(const size_t) const
    {
        return std::nullopt;
    }

    std::optional<size_t> MySqlResult::columnIndex(const std::string_view) const
    {
        return std::nullopt;
    }

    DatabaseValue MySqlResult::getValue(const size_t) const
    {
        return std::monostate{};
    }

    void MySqlResult::reset()
    {
        // 桩里没有游标可复位；清空历史错误文本仍然要做，语义与真实实现保持一致
        m_lastError.clear();
    }

    // 仅为满足头文件里的声明而保留：桩构建里没有可转换的列值字节
    DatabaseValue MySqlResult::convertValue(const char *, const size_t, const size_t) const
    {
        return std::monostate{};
    }

#endif // DATABASE_HAS_MYSQL

    // ------------------------------------------------------------------------
    // 以下定义只依赖构造阶段快照下来的成员，不触碰 MySQL C API，
    // 因此真实实现与桩实现共用同一份定义（桩下列数恒为 0，一切自然退化为空集）
    // ------------------------------------------------------------------------

    size_t MySqlResult::rowCount() const
    {
        // 预读档这里是构造时快照的精确值；流式档恒为 0，含义是「未知」而不是「没有行」，
        // 判口径写在头文件与 isEmpty() 上，这里不多存一份状态
        return m_rowCount;
    }

    size_t MySqlResult::columnCount() const
    {
        return m_columnCount;
    }

    DatabaseValue MySqlResult::getValue(const std::string_view name) const
    {
        // 先按名解析索引再走索引重载，保证两条路径的越界与 NULL 判定完全一致
        const std::optional<size_t> index = columnIndex(name);
        if (!index.has_value())
        {
            // 列名解析失败与列值为 NULL 在契约里同为 monostate，本方法属 const 读取路径，不写错误状态
            return std::monostate{};
        }

        return getValue(*index);
    }

    std::vector<std::string> MySqlResult::columnNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_columnCount);

        for (size_t index = 0; index < m_columnCount; ++index)
        {
            // 复用 columnName()，列名来源只有一处真值；缺名列补空串占位，
            // 保证返回列表长度恒等于 columnCount() 且下标与列序严格对齐
            names.push_back(columnName(index).value_or(std::string{}));
        }

        return names;
    }

    bool MySqlResult::isEmpty() const
    {
        // 预读结果的行数恒为精确值（写回执没有游标，行数也是 0），因此空与非空直接由行数判定，
        // 不再另存一份标志位——同一事实两处真值来源迟早会对不上。
        // 流式档反过来：没读完就没有「这堆行是空的」的依据，一律回 false（不宣称自己空）。
        // 误报「空」会让调用方整段跳过它本该处理的数据，这个方向比白跑一次 next() 贵得多
        if (m_isStreaming)
        {
            return false;
        }

        return m_rowCount == 0;
    }

    std::int64_t MySqlResult::affectedRowCount() const noexcept
    {
        // 只把构造时快照的语句级影响行数交出去：本方法不触碰任何句柄，因此 noexcept 成立。
        // 查询结果集构造时传的是 0，符合基类「只读结果集返回 0」的约定
        return m_affectedRowCount;
    }

    std::int64_t MySqlResult::lastInsertRowId() const noexcept
    {
        // 与影响行数同一条纪律：只交构造时就快照好的语句级值，本方法不碰句柄，因此 noexcept 成立。
        // 桩构建下这条从未被写过，恒为 0——桩里 connect() 必失败，报出非零标识只会是假信息
        return m_lastInsertRowId;
    }

} // namespace AsynGyanis::Database
