/**
 * @file RedisResult.h
 * @brief Redis 命令结果集实现
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Database/Common/DatabaseResult.h"
#include "Database/Redis/RedisConnection.h" // 复用其中全局作用域的 redisReply 前置声明

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Database
{
    /**
     * @brief Redis 命令结果集
     *
     * @details 持有 hiredis 交出的一个 redisReply（所有权归本对象，析构时 freeReplyObject），把一条完整
     *          回复看成「一行」、回复的元素个数看成「列数」：标量 1 行 1 列、数组 1 行 N 列、
     *          nil 与空数组 0 行 0 列，故 rowCount() 恒为 1 或 0，isEmpty() 等价于 rowCount()==0。
     *
     * @note 与 SQLite 驱动的差异（必须说清）：SQLite 的游标挂在服务端预编译语句上，必须先 next()
     *       才能读列值；Redis 数据已在内存里，本类不 gate 任何数据可读性。
     * @note 顶层元素映射：STRING / STATUS / ERROR → std::string（按长度拷贝，内嵌 '\0' 不丢失）、
     *       INTEGER → std::int64_t、NIL 或未知类型 → std::monostate、ARRAY → std::vector<std::string>；
     *       HGETALL / HMGET 在 RESP2 下是「字段、值」交替的扁平数组，与普通字符串数组在协议上无从区分，
     *       因此一律按列表交出，由调用方自行两两配对，本类不映射成哈希备选类型。
     *
     * @warning Redis 没有列名概念，columnName()/columnNames() 只能给出按下标合成的名字
     *          （"value0"、"value1"…），它不承载任何语义，也不等于哈希字段名；
     *          稳定可靠的取值方式是按下标 getValue(index)。
     * @warning 数组转 std::vector<std::string> 是有损映射：DatabaseValue 的列表备选类型只有
     *          字符串一种，因此嵌套元素里的整数与浮点数一律按十进制文本保留（见 convertReply()），
     *          不会被静默丢弃。
     */
    class RedisResult : public DatabaseResult
    {
    public:
        /**
         * @brief 用 hiredis 的回复构造结果集并接管其所有权
         * @details 构造阶段一次性快照回复类型与列数（元素个数），之后所有接口只读这些缓存；
         *          传 nullptr 表示「没有回复」，按空结果集处理，析构时也不会调用 freeReplyObject。
         *          error 类型的回复会在构造这一非 const 写路径上把服务端原文摘进 m_lastError，
         *          使 lastError() 有内容可返回；const 读取路径不再改写它。
         * @param ownedReply hiredis 交出的 redisReply 指针，所有权移交本对象；可为 nullptr
         */
        explicit RedisResult(redisReply *ownedReply);

        /**
         * @brief 析构时释放所持有的 redisReply（含其全部嵌套子元素）
         */
        ~RedisResult() override;

        // 回复指针所有权唯一：拷贝会导致同一个 redisReply 被 freeReplyObject 两次；
        // 移动则让源对象析构时再次释放已经交给目标对象的回复。基类同样删除了拷贝与移动。
        RedisResult(const RedisResult &) = delete;

        RedisResult &operator=(const RedisResult &) = delete;

        RedisResult(RedisResult &&) = delete;

        RedisResult &operator=(RedisResult &&) = delete;

        /**
         * @brief 将游标移动到下一行
         * @details 重写 DatabaseResult::next()：本类只有一行（见类注释的退化单行契约），
         *          因此首次调用且结果非空时返回 true，其后恒为 false；空回复直接返回 false。
         *          与基类的差异：不推进任何真实游标（数据已全部在内存里），只翻转一个
         *          「唯一一行是否已交出」的标志，reset() 会把它复位从而支持重新遍历。
         *          按基类契约本方法属只读路径，即使重复调用也不会改写 m_lastError。
         * @return true 游标停在这一行上，可以读取列值
         * @return false 已经走过唯一一行，或结果集本就为空
         */
        bool next() override;

        /**
         * @brief 获取结果集行数
         * @details 重写 DatabaseResult::rowCount()：Redis 回复一次性到达，行数只可能是 1 或 0，
         *          不存在 SQLite 那种「无法预先得知全部行」的情形，因此不会用 0 表示未知。
         * @return size_t 有数据时为 1，空回复为 0
         */
        [[nodiscard]] size_t rowCount() const override;

        /**
         * @brief 获取结果集列数
         * @details 重写 DatabaseResult::columnCount()：即当前回复的元素个数——
         *          数组回复取其 elements 数量，标量回复视为 1 列，nil 与空数组为 0 列。
         *          数值取构造时的快照，不逐次调用第三方接口。
         * @return size_t 列数
         */
        [[nodiscard]] size_t columnCount() const override;

        /**
         * @brief 按列索引取列名
         * @details 重写 DatabaseResult::columnName()：Redis 协议里回复元素没有名字，
         *          这里只能返回按下标合成的无语义名字 "value" + 下标（"value0"、"value1"…），
         *          目的是让依赖列名的通用遍历代码不至于完全不可用。
         * @param index 列索引，从 0 开始
         * @return std::optional<std::string> 合成列名；索引越界（含空结果集）返回空值
         */
        [[nodiscard]] std::optional<std::string> columnName(size_t index) const override;

        /**
         * @brief 按列名取列索引
         * @details 重写 DatabaseResult::columnIndex()：在合成的列名上做精确匹配的顺序扫描，
         *          与 columnName() 严格互逆；因为合成名字本身不带语义，
         *          推荐调用方直接按下标取值而不是按名查找。
         * @param name 列名，需与 "value" + 下标 完全一致（区分大小写）
         * @return std::optional<size_t> 列索引；名字不匹配或结果集为空返回空值
         */
        [[nodiscard]] std::optional<size_t> columnIndex(std::string_view name) const override;

        /**
         * @brief 按列索引读取值
         * @details 重写 DatabaseResult::getValue()：取回复的第 index 个元素并映射成 DatabaseValue，不要求先调用
         *          next()，游标是否推进也不改变取值结果。取值失败（索引越界、无回复、nil、类型未知）一律返回
         *          std::monostate 且绝不写入 m_lastError——这是 const 读取路径，基类不允许它改状态；
         *          服务端报错原文可从 lastError()（构造时摘取）与 isError() 组合判定。
         * @param index 列索引，从 0 开始
         * @return DatabaseValue 列值；索引无效或该元素为 nil 时返回 std::monostate
         */
        [[nodiscard]] DatabaseValue getValue(size_t index) const override;

        /**
         * @brief 按列名读取值
         * @details 重写 DatabaseResult::getValue()：先把合成列名解析成索引，再走索引重载，
         *          保证两条路径的越界与 nil 判定完全一致。
         * @param name 合成列名（"value" + 下标）
         * @return DatabaseValue 列值；列名不匹配或该元素为 nil 时返回 std::monostate
         */
        [[nodiscard]] DatabaseValue getValue(std::string_view name) const override;

        /**
         * @brief 获取全部列名
         * @details 重写 DatabaseResult::columnNames()：长度恒等于 columnCount()，
         *          元素为按下标合成的无语义名字，与 columnName() 逐项一致。
         * @return std::vector<std::string> 合成列名列表；空结果集时为空向量
         */
        [[nodiscard]] std::vector<std::string> columnNames() const override;

        /**
         * @brief 重置游标到唯一那一行之前
         * @details 重写 DatabaseResult::reset()：只复位「唯一一行是否已交出」标志，使 next() 可以重新返回 true，
         *          因数据在内存中，本方法不影响任何已读出的值，也不释放回复。
         */
        void reset() override;

        /**
         * @brief 判断结果集是否为空
         * @details 重写 DatabaseResult::isEmpty()：等价于 rowCount() == 0，
         *          即「无回复 / nil 回复 / 空数组」。标量 0 或空字符串回复都不算空——
         *          那是服务端确实给出的值，与「没有值」必须区分开。
         * @return true 没有任何数据行
         */
        [[nodiscard]] bool isEmpty() const override;

        /**
         * @brief 判断当前回复是否为服务端 error 回复
         * @details 单命令路径（RedisConnection::execute）已经把 error 回复转成 nullptr + lastError()，
         *          因此本方法主要服务于管道路径：flushPipeline() 会把每一条 error 回复原样封装交回，
         *          调用方用它逐条定位失败命令。
         * @return true 回复类型为 REDIS_REPLY_ERROR，错误原文见 lastError()
         */
        [[nodiscard]] bool isError() const;

        /**
         * @brief 获取回复类型标识
         * @details 返回 hiredis 的 REDIS_REPLY_* 常量原值，供确实需要区分状态/整数/数组的高级代码使用；
         *          要解读数值请配合 nativeHandle() 自行包含 <hiredis/hiredis.h>。
         * @return int REDIS_REPLY_* 常量；0 表示没有回复（含未编译 hiredis 的桩构建）
         */
        [[nodiscard]] int replyType() const noexcept
        {
            return m_replyType;
        }

        /**
         * @brief 获取底层 redisReply 指针，供需要直接遍历嵌套回复的高级场景使用
         * @warning 所有权仍属于本结果集，调用方不得 freeReplyObject，也不得在结果集销毁后继续使用
         * @return redisReply* 无回复时为 nullptr
         */
        [[nodiscard]] const redisReply *nativeHandle() const noexcept
        {
            return m_replyPointer;
        }

    private:
        /**
         * @brief 把单个 redisReply 递归映射为统一的 DatabaseValue
         * @details 纯函数：只读回复，不写 m_lastError，因此可以安全地在 const 取值路径上调用。
         *          嵌套数组的每个子元素都会被转成文本塞进 std::vector<std::string>
         *          （整数转十进制、浮点取服务端原始文本），这是列表备选类型只有字符串导致的有损映射，
         *          但比「非字符串子元素直接丢弃」更能保住信息。
         * @param sourceReply 待转换的回复节点，可为 nullptr
         * @return DatabaseValue 映射后的值；空节点、nil 或未知类型返回 std::monostate
         */
        [[nodiscard]] DatabaseValue convertReply(const redisReply *sourceReply) const;

        redisReply *m_replyPointer{nullptr}; ///< hiredis 回复指针，非空时由本对象负责 freeReplyObject
        int         m_replyType{0};          ///< 回复类型快照（REDIS_REPLY_* 常量原值），0 表示无回复
        size_t      m_columnCount{0};        ///< 列数快照，即回复的元素个数
        bool        m_hasReturnedRow{false}; ///< 唯一那一行是否已被 next() 交出，reset() 复位
    };

} // namespace AsynGyanis::Database
