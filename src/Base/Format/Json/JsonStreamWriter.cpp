/**
 * @file JsonStreamWriter.cpp
 * @brief JSON 流式写出：按事件顺序逐块拼装文本，与 DOM 版 JsonWriter 逐字节一致
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Format/Json/JsonStreamWriter.h"

#include "Base/Format/Json/JsonWriter.h"
#include "Base/Format/FormatError.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace AsynGyanis::Base
{
    JsonStreamWriter::JsonStreamWriter(const JsonWriteOptions &options) :
        m_options(options)
    {
    }

    void JsonStreamWriter::beginObject()
    {
        beginContainer(true);
    }

    void JsonStreamWriter::endObject()
    {
        endContainer(true);
    }

    void JsonStreamWriter::beginArray()
    {
        beginContainer(false);
    }

    void JsonStreamWriter::endArray()
    {
        endContainer(false);
    }

    void JsonStreamWriter::writeKey(const std::string_view key)
    {
        if (m_stack.empty() || !m_stack.back().isObject)
        {
            throw FormatError(FormatErrorKind::UnexpectedByte, "writeKey 只能用于对象内部", TextPosition{});
        }
        if (m_expectingValueAfterKey)
        {
            throw FormatError(FormatErrorKind::UnexpectedByte, "上一个对象成员的值尚未写出", TextPosition{});
        }

        prepareElementWrite();
        // 键与字符串值共用 JsonWriter 的转义实现，确保与 DOM 写出逐字节一致
        m_output += JsonWriter::write(FormatValue(std::string(key)), m_options);
        m_output += m_options.indentWidth > 0 ? ": " : ":";
        m_expectingValueAfterKey = true;
    }

    void JsonStreamWriter::writeString(const std::string_view text)
    {
        prepareValueWrite();
        m_output += JsonWriter::write(FormatValue(std::string(text)), m_options);
    }

    void JsonStreamWriter::writeNumber(const double value)
    {
        prepareValueWrite();
        // 非有限浮点没有合法的 JSON 表示，JsonWriter 会写出 null，这里同样如此
        m_output += JsonWriter::write(FormatValue(value), m_options);
    }

    void JsonStreamWriter::writeNumber(const std::int64_t value)
    {
        prepareValueWrite();
        m_output += JsonWriter::write(FormatValue(value), m_options);
    }

    void JsonStreamWriter::writeNumber(const std::uint64_t value)
    {
        prepareValueWrite();
        m_output += JsonWriter::write(FormatValue(value), m_options);
    }

    void JsonStreamWriter::writeNumber(const std::string_view rawNumber)
    {
        // 与 JsonParser 相同的取值级联：非负整数 int64_t → uint64_t → double，
        // 负整数 int64_t → double，从而与 DOM 侧的数值类型判定保持一致
        std::int64_t signedInteger = 0;
        if (const auto [pointer, errorCode] = std::from_chars(rawNumber.data(), rawNumber.data() + rawNumber.size(), signedInteger);
            errorCode == std::errc() && pointer == rawNumber.data() + rawNumber.size())
        {
            writeNumber(signedInteger);
            return;
        }

        if (rawNumber.empty() || rawNumber.front() != '-')
        {
            std::uint64_t unsignedInteger = 0;
            if (const auto [pointer, errorCode] = std::from_chars(rawNumber.data(), rawNumber.data() + rawNumber.size(), unsignedInteger);
                errorCode == std::errc() && pointer == rawNumber.data() + rawNumber.size())
            {
                writeNumber(unsignedInteger);
                return;
            }
        }

        double floatingPoint = 0.0;
        if (const auto [pointer, errorCode] = std::from_chars(rawNumber.data(), rawNumber.data() + rawNumber.size(), floatingPoint);
            errorCode == std::errc() && pointer == rawNumber.data() + rawNumber.size())
        {
            // JSON 数字不接受 nan/inf；from_chars 可能把它们解析成功，这里显式拒绝
            if (!std::isfinite(floatingPoint))
            {
                throw FormatError(FormatErrorKind::NumberOutOfRange,
                                  "流式写出的数字不是有限值：" + std::string(rawNumber),
                                  TextPosition{});
            }
            writeNumber(floatingPoint);
            return;
        }

        throw FormatError(FormatErrorKind::InvalidNumber,
                          "流式写出的数字原文非法：" + std::string(rawNumber),
                          TextPosition{});
    }

    void JsonStreamWriter::writeBool(const bool value)
    {
        prepareValueWrite();
        m_output += value ? "true" : "false";
    }

    void JsonStreamWriter::writeNull()
    {
        prepareValueWrite();
        m_output += "null";
    }

    const std::string &JsonStreamWriter::output() const noexcept
    {
        return m_output;
    }

    std::string JsonStreamWriter::takeOutput()
    {
        std::string output = std::move(m_output);
        reset();
        return output;
    }

    std::size_t JsonStreamWriter::depth() const noexcept
    {
        return m_stack.size();
    }

    void JsonStreamWriter::reset() noexcept
    {
        m_stack.clear();
        m_output.clear();
        m_expectingValueAfterKey = false;
        m_rootValueWritten       = false;
    }

    void JsonStreamWriter::prepareElementWrite()
    {
        if (m_stack.empty())
        {
            // 根值不写分隔符也不写缩进
            return;
        }

        const bool      isIndented = m_options.indentWidth > 0;
        ContainerFrame &frame      = m_stack.back();

        // 与 JsonWriter 对齐：首元素前写换行，后续元素前写逗号加换行，再写本层缩进
        m_output += frame.elementCount == 0 ? (isIndented ? "\n" : "") : (isIndented ? ",\n" : ",");
        appendIndentation(isIndented ? m_stack.size() : 0);
        ++frame.elementCount;
    }

    void JsonStreamWriter::prepareValueWrite()
    {
        // 对象成员：分隔符与缩进已在 writeKey 时写出，这里只消费标记，避免重复写分隔符
        if (m_expectingValueAfterKey)
        {
            m_expectingValueAfterKey = false;
            return;
        }

        if (!m_stack.empty() && m_stack.back().isObject)
        {
            throw FormatError(FormatErrorKind::UnexpectedByte,
                              "对象成员的值必须紧跟 writeKey 之后写出",
                              TextPosition{});
        }

        if (m_stack.empty())
        {
            // 根值只能有一个：写出第二次会在同一份文本里拼出两段文档
            if (m_rootValueWritten)
            {
                throw FormatError(FormatErrorKind::UnexpectedByte, "文档根值只能写出一次", TextPosition{});
            }
            m_rootValueWritten = true;
        }

        prepareElementWrite();
    }

    void JsonStreamWriter::appendIndentation(const std::size_t level)
    {
        if (m_options.indentWidth == 0)
        {
            return;
        }
        m_output.append(level * m_options.indentWidth, ' ');
    }

    void JsonStreamWriter::beginContainer(const bool isObject)
    {
        prepareValueWrite();

        // 深度守护与 JsonWriter 一致：容器所在层数 = 当前栈深，根容器为第 0 层
        if (m_options.maximumDepth != 0 && m_stack.size() >= m_options.maximumDepth)
        {
            throw FormatError(FormatErrorKind::DepthExceeded, "序列化嵌套深度超出上限", TextPosition{});
        }

        m_output += isObject ? '{' : '[';
        m_stack.push_back(ContainerFrame{.isObject = isObject, .elementCount = 0});
    }

    void JsonStreamWriter::endContainer(const bool isObject)
    {
        if (m_stack.empty() || m_stack.back().isObject != isObject)
        {
            throw FormatError(FormatErrorKind::UnexpectedByte, "容器结束调用与当前层级不匹配", TextPosition{});
        }
        if (m_expectingValueAfterKey)
        {
            throw FormatError(FormatErrorKind::UnexpectedByte, "对象成员的值尚未写出", TextPosition{});
        }

        const ContainerFrame frame = m_stack.back();
        m_stack.pop_back();

        // 空容器恒为 "[]"/"{}"（不换行），与 JsonWriter 的写法一致
        if (frame.elementCount > 0 && m_options.indentWidth > 0)
        {
            m_output += '\n';
            // 弹栈后栈深即为该容器的层数，收尾括号与容器起始位置对齐
            appendIndentation(m_stack.size());
        }
        m_output += isObject ? '}' : ']';
    }
} // namespace AsynGyanis::Base
