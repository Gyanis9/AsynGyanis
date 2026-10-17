// log_export 配置段：启用时的必填项、数值形状与未知键拒绝

#include "Net/Logging/LogExportConfiguration.h"

#include "Base/Exception/ConfigValidationException.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 造一份最小可用的 log_export 段
        Base::ConfigValue makeRoot()
        {
            Base::ConfigValue otlp = Base::ConfigValue::object();
            otlp["endpoint"]       = "http://127.0.0.1:4318/v1/logs";
            otlp["timeout_ms"]     = 1500;

            Base::ConfigValue section        = Base::ConfigValue::object();
            section["enabled"]               = true;
            section["service_name"]          = "asyngyanis";
            section["export_interval_ms"]    = 2000;
            section["maximum_batch_records"] = 16;
            section["otlp"]                  = std::move(otlp);

            Base::ConfigValue root = Base::ConfigValue::object();
            root["log_export"]     = std::move(section);
            return root;
        }
    } // namespace

    TEST(LogExportConfiguration, ReadsEnabledSection)
    {
        const LogExportConfiguration configuration = readLogExportConfiguration(makeRoot());
        EXPECT_TRUE(configuration.enabled);
        EXPECT_EQ(configuration.serviceName, "asyngyanis");
        EXPECT_EQ(configuration.otlpEndpoint, "http://127.0.0.1:4318/v1/logs");
        EXPECT_EQ(configuration.exportInterval, std::chrono::milliseconds{2000});
        EXPECT_EQ(configuration.requestTimeout, std::chrono::milliseconds{1500});
        EXPECT_EQ(configuration.maximumBatchRecordCount, 16U);
    }

    TEST(LogExportConfiguration, RejectsUnknownKeys)
    {
        Base::ConfigValue root                = makeRoot();
        root["log_export"]["export_interval"] = 5000; // 少写 _ms
        EXPECT_THROW(static_cast<void>(readLogExportConfiguration(root)), AsynGyanis::Base::ConfigValidationException);
    }

    TEST(LogExportConfiguration, RequiresServiceNameAndEndpointWhenEnabled)
    {
        Base::ConfigValue noService = makeRoot();
        noService["log_export"].erase("service_name");
        EXPECT_THROW(static_cast<void>(readLogExportConfiguration(noService)), AsynGyanis::Base::ConfigValidationException);

        Base::ConfigValue noEndpoint = makeRoot();
        noEndpoint["log_export"]["otlp"].erase("endpoint");
        EXPECT_THROW(static_cast<void>(readLogExportConfiguration(noEndpoint)), AsynGyanis::Base::ConfigValidationException);
    }

    TEST(LogExportConfiguration, DisabledSectionIgnoresValuesButStillRejectsUnknownKeys)
    {
        Base::ConfigValue root        = Base::ConfigValue::object();
        root["log_export"]            = Base::ConfigValue::object();
        root["log_export"]["enabled"] = false;
        EXPECT_FALSE(readLogExportConfiguration(root).enabled);

        root["log_export"]["bogus"] = 1;
        EXPECT_THROW(static_cast<void>(readLogExportConfiguration(root)), AsynGyanis::Base::ConfigValidationException);
    }
} // namespace AsynGyanis::Net
