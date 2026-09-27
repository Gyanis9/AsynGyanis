#include "Net/Tracing/FileSpanExporter.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Exception/SystemException.h"
#include "Platform/FileSystem/FileSystem.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 节的角色折成小写单词
         * @details 文件里写文本而不是 OTLP 那个数字：这一路是给人在 `rg`/`jq` 里看的，
         *          "kind":"server" 不用查表就懂，而 OTLP 出口那边要保持数字形态（那边的读者是解码器）。
         */
        constexpr std::string_view spanKindTextOf(const SpanKind kind) noexcept
        {
            switch (kind)
            {
                case SpanKind::Server:
                    return "server";
                case SpanKind::Client:
                    return "client";
                case SpanKind::Internal:
                    break;
            }
            return "internal";
        }

        /// @brief 结局折成小写单词；Unset 用 "unset" 而不是省略，好让「没判定」这一档查得出来
        constexpr std::string_view spanStatusTextOf(const SpanStatusCode status) noexcept
        {
            switch (status)
            {
                case SpanStatusCode::Ok:
                    return "ok";
                case SpanStatusCode::Error:
                    return "error";
                case SpanStatusCode::Unset:
                    break;
            }
            return "unset";
        }

        /// @brief 一条节折成自描述的一行 JSON（键序由 nlohmann 的映射决定，逐行比对时看值不看序）
        Base::ConfigValue lineValueOf(const TraceResource &resource, const SpanRecord &record)
        {
            Base::ConfigValue line = Base::ConfigValue::object();
            line["traceId"]        = record.identity.traceIdText();
            line["spanId"]         = record.identity.spanIdText();
            line["parentSpanId"]   = record.identity.parentSpanIdText();
            line["name"]           = record.name;
            line["kind"]           = spanKindTextOf(record.kind);
            line["serviceName"]    = resource.serviceName;
            if (!resource.serviceVersion.empty())
            {
                line["serviceVersion"] = resource.serviceVersion;
            }

            const auto startNanoseconds = std::chrono::floor<std::chrono::nanoseconds>(record.startMoment.time_since_epoch()).count();
            line["startTimeUnixNano"]   = std::to_string(startNanoseconds);
            // 时长用单调钟量得的纳秒数直写：墙钟被回拨时结束时刻会早于开始时刻，只有这一列能说明白
            line["durationNano"] = std::to_string(record.duration.count());
            line["status"]       = spanStatusTextOf(record.status);
            if (!record.statusMessage.empty())
            {
                line["statusMessage"] = record.statusMessage;
            }

            Base::ConfigValue attributes = Base::ConfigValue::object();
            for (const SpanAttribute &attribute: record.attributes)
            {
                // 维度写成对象而不是 KeyValue 数组：键在本框架内保证唯一（同名是换值不是追加），
                // 而 jq 里 `.attributes["http.request.method"]` 比翻数组省事
                std::visit([&attributes, &key = attribute.key](const auto &rawValue) { attributes[key] = rawValue; }, attribute.value);
            }
            line["attributes"] = std::move(attributes);
            if (record.droppedAttributeCount != 0U)
            {
                line["droppedAttributesCount"] = record.droppedAttributeCount;
            }
            return line;
        }
    } // namespace

    FileSpanExporter::FileSpanExporter(std::filesystem::path filePath) : m_filePath(std::move(filePath))
    {
        // 父目录先建出来：让运维改一行配置就能把链路写到没建过的子目录里，不必自己先 mkdir
        const std::filesystem::path parentDirectory = m_filePath.parent_path();
        if (!parentDirectory.empty())
        {
            std::error_code directoryError;
            std::filesystem::create_directories(parentDirectory, directoryError);
            if (directoryError && !std::filesystem::is_directory(parentDirectory))
            {
                throw Base::SystemException("创建链路文件的目录失败：" + Platform::FileSystem::utf8FromPath(parentDirectory));
            }
        }

        // 二进制模式 + 追加：行尾由本类补 "\n"，不让文本模式在 Windows 上偷偷翻成 "\r\n"
        m_file.open(m_filePath, std::ios::out | std::ios::app | std::ios::binary);
        if (!m_file.is_open())
        {
            throw Base::SystemException("无法打开链路文件：" + Platform::FileSystem::utf8FromPath(m_filePath) + "。请检查路径与写权限");
        }
    }

    FileSpanExporter::~FileSpanExporter()
    {
        shutdown();
    }

    bool FileSpanExporter::exportSpans(const TraceResource &resource, const std::vector<SpanRecord> &spans)
    {
        const std::lock_guard lock(m_writeMutex);
        if (!m_file.is_open())
        {
            // 已经被 shutdown()：出口整批没收，让 Tracer 的计数把这件事报出来，而不是假装收下
            return false;
        }

        for (const SpanRecord &record: spans)
        {
            const auto line = Base::serializeConfigValue(lineValueOf(resource, record));
            if (!line.has_value())
            {
                // 正文没能成形（含非法 UTF-8 的取值）：后面的行不再试，整批判失败
                return false;
            }
            m_file << *line << '\n';
            if (!m_file.good())
            {
                return false;
            }
        }

        // 一批一次刷新：每条一节各刷一次盘会让出口线程整个排在磁盘后面
        m_file.flush();
        return m_file.good();
    }

    std::string_view FileSpanExporter::exporterName() const noexcept
    {
        return "file";
    }

    void FileSpanExporter::shutdown() noexcept
    {
        const std::lock_guard lock(m_writeMutex);
        if (m_file.is_open())
        {
            m_file.flush();
            m_file.close();
        }
    }

    const std::filesystem::path &FileSpanExporter::filePath() const noexcept
    {
        return m_filePath;
    }
} // namespace AsynGyanis::Net
