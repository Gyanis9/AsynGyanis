/**
 * @file BaseTestSupport.h
 * @brief Base 模块单元测试辅助：转发共享的临时目录、等待与时刻折算夹具
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 实体定义在 tests/TestSupport/CommonTestSupport.h（该头不依赖任何模块的生产代码）；
 *          这里以 using 转发进 Base::TestSupport，既有调用点无需改动。
 */

#pragma once

#include "CommonTestSupport.h"

namespace AsynGyanis::Base::TestSupport
{
    using AsynGyanis::TestSupport::hasResolvedStackTraceFrames;
    using AsynGyanis::TestSupport::kWaitTimeout;
    using AsynGyanis::TestSupport::makeLocalMoment;
    using AsynGyanis::TestSupport::TemporaryDirectory;
    using AsynGyanis::TestSupport::waitForCondition;
} // namespace AsynGyanis::Base::TestSupport
