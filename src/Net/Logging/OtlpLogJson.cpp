#include "Net/Logging/OtlpLogJson.h"

#include "Net/Telemetry/OtlpJsonSupport.h"
#include "Base/Config/ConfigValue.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    namespace
    {
        using Telemetry::keyValueOf;
        using Telemetry::textValueOf;
        using Telemetry::unixNanoTextOf;

        /// @brief OTLP 的 severityNumber：取各档的起点值（TRACE=1、DEBUG=5、INFO=9、WARN=13、ERROR=17、FATAL=21）
        int severityNumber(const Base::LogLevel level) noexcept
        {
            switch (level)
            {
                case Base::LogLevel::Trace:
                    return 1;
                case Base::LogLevel::Debug:
                    return 5;
                case Base::LogLevel::Info:
                    return 9;
                case Base::LogLevel::Warn:
                    return 13;
                case Base::LogLevel::Error:
                    return 17;
                case Base::LogLevel::Fatal:
                    return 21;
                default:
                    return 9;
            }
        }

        /// @brief severityText：去掉文本版式的对齐空格，JSON 里只留等级名
        std::string severityTextOf(const Base::LogLevel level)
        {
            std::string text{Base::logLevelToString(level)};
            while (!text.empty() && text.back() == ' ')
            {
                text.pop_back();
            }
            return text;
        }

        /// @brief 一条记录折成 OTLP 的 LogRecord
        Base::ConfigValue recordObjectOf(const OtlpLogRecord &record)
        {
            Base::ConfigValue object = Base::ConfigValue::object();
            object["timeUnixNano"]   = unixNanoTextOf(record.timestamp);
            object["severityText"]   = severityTextOf(record.level);
            object["severityNumber"] = severityNumber(record.level);
            object["body"]           = textValueOf(record.message);

            Base::ConfigValue attributes = Base::ConfigValue::array();
            if (!record.loggerName.empty())
            {
                attributes.push_back(keyValueOf("logger.name", textValueOf(record.loggerName)));
            }
            if (!record.threadId.empty())
            {
                attributes.push_back(keyValueOf("thread.id", textValueOf(record.threadId)));
            }
            for (const auto &[key, value]: record.attributes)
            {
                attributes.push_back(keyValueOf(key, textValueOf(value)));
            }
            object["attributes"] = std::move(attributes);
            return object;
        }
    } // namespace

    std::string formatOtlpLogsJson(const TraceResource &resource, const std::vector<OtlpLogRecord> &records)
    {
        Base::ConfigValue scope = Base::ConfigValue::object();
        scope["name"]           = kLoggingScopeName;

        Base::ConfigValue recordArray = Base::ConfigValue::array();
        for (const OtlpLogRecord &record: records)
        {
            recordArray.push_back(recordObjectOf(record));
        }

        Base::ConfigValue scopeLogs = Base::ConfigValue::object();
        scopeLogs["scope"]          = std::move(scope);
        scopeLogs["logRecords"]     = std::move(recordArray);

        Base::ConfigValue resourceAttributes = Base::ConfigValue::array();
        resourceAttributes.push_back(keyValueOf("service.name", textValueOf(resource.serviceName)));
        if (!resource.serviceVersion.empty())
        {
            resourceAttributes.push_back(keyValueOf("service.version", textValueOf(resource.serviceVersion)));
        }

        Base::ConfigValue resourceObject = Base::ConfigValue::object();
        resourceObject["attributes"]     = std::move(resourceAttributes);

        Base::ConfigValue resourceLogs = Base::ConfigValue::object();
        resourceLogs["resource"]       = std::move(resourceObject);
        resourceLogs["scopeLogs"]      = Base::ConfigValue::array({std::move(scopeLogs)});

        Base::ConfigValue document = Base::ConfigValue::object();
        document["resourceLogs"]   = Base::ConfigValue::array({std::move(resourceLogs)});

        return Base::serializeConfigValue(document).value_or(std::string{});
    }
} // namespace AsynGyanis::Net
