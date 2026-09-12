/**
 * @file JsonStreamWriter.h
 * @brief JSON 流式写出：按事件顺序逐块拼装文本，与 DOM 版 JsonWriter 逐字节一致
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Format/Json/JsonWriteOptions.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Base
{
    /**
     * @brief JSON 流式写出器
     *
     * @details 与 DOM 版 JsonWriter 的关系与取舍：
     *          - JsonWriter 递归遍历 FormatValue 一次性产出完整文本，写法简单，适合已有 DOM 的场景；
     *          - JsonStreamWriter 只按调用顺序拼装，**不持有 DOM**，因此可以把「读一半、写一半」
     *            的管道接起来（JsonReader 出一个事件就写一个），内存占用与文档规模无关；
     *          - 两者的标量格式（字符串转义、数字最短往返表示、非有限数写 null）完全共用
     *            JsonWriter 的公开实现，容器标点与缩进规则也逐条对齐，因此「同一份结构，
     *            一个走 DOM、一个走事件流」得到的结果**逐字节相同**（见 TestJsonStream.cpp 的交叉验证）。
     *
     * 复用 JsonWriteOptions：
     *          - `indentWidth`：0 为紧凑单行，正数为每层空格数，空容器恒为 `[]`/`{}`；
     *          - `ensureAscii`：非 ASCII 字符转 `\uXXXX`（补充平面用代理对）；
     *          - `maximumDepth`：容器深度守卫，超限抛 FormatError（kind 为 DepthExceeded）；
     *          - 对象键序：**流式写出不参与重排**——写出顺序完全由调用顺序决定，
     *            无法在只看到部分成员时决定整体顺序。要得到与 DOM 写出逐字节一致的结果，
     *            调用方需按升序提供键（FormatValueObject 本身按键升序，因此从 DOM 顺序
     *            驱动事件时天然满足）。
     *
     * 使用约束：
     *          - 每个值写出前必须处于合法位置：数组内、对象成员的值位置（紧跟 writeKey 之后）
     *            或文档根（根值只能写一次）；
     *          - 容器必须成对闭合，否则 endXxx() 会抛错；
     *          - 抛错后缓冲区内容不再可信，请 reset() 后重来。
     */
    class JsonStreamWriter
    {
    public:
        /**
         * @brief 构造流式写出器
         * @param options 序列化选项
         */
        explicit JsonStreamWriter(const JsonWriteOptions &options = JsonWriteOptions{});

        /**
         * @brief 析构函数
         */
        ~JsonStreamWriter() = default;

        /**
         * @brief 移动构造函数
         * @details 转移输出缓冲与层级状态；源对象退化为可析构但不可再用。
         * @param other 待移源的写出器
         */
        JsonStreamWriter(JsonStreamWriter &&other) noexcept = default;

        /**
         * @brief 移动赋值运算符
         * @details 释放自身状态后接管源对象；自移动安全。
         * @param other 待移源的写出器
         * @return JsonStreamWriter& 本对象引用
         */
        JsonStreamWriter &operator=(JsonStreamWriter &&other) noexcept = default;

        // 写出器持有输出的层级状态，拷贝会产生两份各自追加、彼此无关的文本，语义不清晰，故禁止
        JsonStreamWriter(const JsonStreamWriter &) = delete;

        JsonStreamWriter &operator=(const JsonStreamWriter &) = delete;

        /**
         * @brief 开始一个对象
         * @throws FormatError 当前不在可写值的位置，或嵌套深度超出 maximumDepth
         */
        void beginObject();

        /**
         * @brief 结束当前对象
         * @throws FormatError 当前层级不是对象，或有成员的值尚未写出
         */
        void endObject();

        /**
         * @brief 开始一个数组
         * @throws FormatError 当前不在可写值的位置，或嵌套深度超出 maximumDepth
         */
        void beginArray();

        /**
         * @brief 结束当前数组
         * @throws FormatError 当前层级不是数组
         */
        void endArray();

        /**
         * @brief 写出一个对象成员名
         * @details 键必须紧随其后跟一个值（beginXxx 或 writeXxx），否则后续 endObject() 会报错。
         * @param key 成员名（未转义原文，内部按键字符串规则转义）
         * @throws FormatError 当前不在对象成员位置，或上一个成员的值尚未写出
         */
        void writeKey(std::string_view key);

        /**
         * @brief 写出一个字符串值
         * @param text 字符串内容（未转义原文）
         * @throws FormatError 当前不在可写值的位置（例如缺少 writeKey）
         */
        void writeString(std::string_view text);

        /**
         * @brief 写出一个浮点数值
         * @details 使用与 JsonWriter 相同的最短往返表示；非有限的浮点输出为 null。
         * @param value 浮点值
         * @throws FormatError 当前不在可写值的位置
         */
        void writeNumber(double value);

        /**
         * @brief 写出一个有符号整数值
         * @param value 整数值
         * @throws FormatError 当前不在可写值的位置
         */
        void writeNumber(std::int64_t value);

        /**
         * @brief 写出一个无符号整数值
         * @param value 无符号整数值
         * @throws FormatError 当前不在可写值的位置
         */
        void writeNumber(std::uint64_t value);

        /**
         * @brief 写出一个数字原文
         * @details 供 JSON 流式读取器直接转发 Number 事件：原文先按
         *          「int64 → uint64 → double」解析成与 DOM 解析器相同的数值类型，
         *          再按键入重载规范化写出。例如原文 `1e2` 会写出 `100.0`，
         *          与 DOM 侧 Double(100.0) 的写出结果一致。
         * @param rawNumber 数字原文（如 `-1.5e3`）
         * @throws FormatError 原文不是合法数字或超出可表示范围
         */
        void writeNumber(std::string_view rawNumber);

        /**
         * @brief 写出一个布尔值
         * @param value 布尔值
         * @throws FormatError 当前不在可写值的位置
         */
        void writeBool(bool value);

        /**
         * @brief 写出 null
         * @throws FormatError 当前不在可写值的位置
         */
        void writeNull();

        /**
         * @brief 获取当前已拼装的文本
         * @return const std::string& 输出缓冲引用，写出过程中的任意时刻都可读取（可能是不完整文档）
         */
        [[nodiscard]] const std::string &output() const noexcept;

        /**
         * @brief 取走输出文本并复位
         * @details 取出后可以继续用同一对象写出下一份文档。
         * @return std::string 已拼装的文本
         */
        [[nodiscard]] std::string takeOutput();

        /**
         * @brief 获取当前尚未闭合的容器层数
         * @return std::size_t 栈深；0 表示回到文档根层级
         */
        [[nodiscard]] std::size_t depth() const noexcept;

        /**
         * @brief 复位为初始状态
         * @details 清空输出与层级状态，便于复用同一对象；已拼装文本被丢弃。
         */
        void reset() noexcept;

    private:
        /**
         * @brief 一个尚未闭合容器的写出状态
         */
        struct ContainerFrame
        {
            bool        isObject{false}; ///< true 为对象、false 为数组
            std::size_t elementCount{0}; ///< 已写出的元素/成员个数，用于决定是否先写分隔符
        };

        /**
         * @brief 写出值之前的公共准备：分隔符、缩进与元素计数
         * @details 与 JsonWriter 一致：首元素前写 `\n`（缩进模式）、后续元素前写 `,\n`，
         *          再写当前层缩进；根值不写任何分隔符。
         */
        void prepareElementWrite();

        /**
         * @brief 写出值之前的合法性校验
         * @details 消费「键已写出」标记；否则校验所在位置为数组元素或文档根。
         * @throws FormatError 对象成员位置缺少 writeKey，或根值被写出第二次
         */
        void prepareValueWrite();

        /**
         * @brief 写出缩进空白
         * @param level 缩进层级
         */
        void appendIndentation(std::size_t level);

        /**
         * @brief 开始一个容器（beginObject/beginArray 的共同实现）
         * @param isObject true 为对象、false 为数组
         * @throws FormatError 当前不在可写值的位置，或嵌套深度超出 maximumDepth
         */
        void beginContainer(bool isObject);

        /**
         * @brief 结束一个容器（endObject/endArray 的共同实现）
         * @param isObject true 为对象、false 为数组
         * @throws FormatError 层级不匹配或有成员的值尚未写出
         */
        void endContainer(bool isObject);

        JsonWriteOptions            m_options;                       ///< 序列化选项快照
        std::string                 m_output;                        ///< 已拼装的输出文本
        std::vector<ContainerFrame> m_stack;                         ///< 未闭合容器栈
        bool                        m_expectingValueAfterKey{false}; ///< 刚写出键，下一个写出必须是它的值
        bool                        m_rootValueWritten{false};       ///< 根值是否已写出（防止拼出两段文档）
    };
} // namespace AsynGyanis::Base
