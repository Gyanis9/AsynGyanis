#include "Platform/System/CgroupInfo.h"

#include "Platform/IO/FileContents.h"
#include "Platform/Platform.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace AsynGyanis::Platform
{
    namespace
    {
        /// /proc/self/cgroup 在极端 v1 部署里可能有很多行；读满这个长度按「文件异常」处理，不拿截断内容猜层级
        constexpr std::size_t kSelfCgroupProbeLength = 8192;

        /// cgroup 的读数文件只有一行十进制或 max，留 256 字节足够；读满同样按「文件异常」处理
        constexpr std::size_t kCgroupValueProbeLength = 256;

        /// v1 的 memory.limit_in_bytes 用接近 2^63 的值表示「不设限」（实际取值随页大小略有差异）
        constexpr std::uint64_t kLegacyUnlimitedMemoryBytes = std::uint64_t{1} << 62;

        /**
         * @brief 一个控制器在本进程 cgroup 层级里的位置
         */
        struct ControllerLocation
        {
            bool        isUnified = false; ///< true 表示在 v2 统一层级；false 表示 v1 的独立控制器层级
            std::string relativePath;      ///< /proc/self/cgroup 里给出的控制组路径，以 '/' 开头
        };

        /**
         * @brief 由 /proc/self/cgroup 解析出的控制器位置表
         */
        struct CgroupLayout
        {
            std::optional<ControllerLocation> memory;                ///< memory 控制器的位置
            std::optional<ControllerLocation> pids;                  ///< pids 控制器的位置
            bool                              hasUnifiedHierarchy{}; ///< 是否出现 v2 的 0:: 行
            std::string                       unifiedRelativePath;   ///< v2 统一层级的控制组路径
        };

        /**
         * @brief 去掉文本两端的 ASCII 空白
         * @param text 待处理文本
         * @return std::string_view 不含前后空白的视图
         */
        std::string_view trimmedAsciiWhitespace(std::string_view text) noexcept
        {
            const auto isWhitespace = [](const char character) noexcept { return character == ' ' || character == '\t' || character == '\r' || character == '\n'; };
            while (!text.empty() && isWhitespace(text.front()))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && isWhitespace(text.back()))
            {
                text.remove_suffix(1);
            }
            return text;
        }

        /**
         * @brief 读一个 cgroup 读数文件的全文
         * @param filePath 文件路径
         * @return std::optional<std::string> 文件内容；打不开、空文件或长度异常时为 nullopt
         * @note 读满探测长度按失败处理：截断的十进制仍能被 from_chars 解析，静默取用会报出一个错的值
         */
        std::optional<std::string> readCgroupValueFile(const std::filesystem::path &filePath) noexcept
        {
            const std::expected<std::string, std::error_code> contents = readFileContents(filePath, 0, kCgroupValueProbeLength);
            if (!contents.has_value() || contents->empty() || contents->size() >= kCgroupValueProbeLength)
            {
                return std::nullopt;
            }
            return *contents;
        }

        /**
         * @brief 解析一个非负十进制数（整段必须是数字）
         * @param text 待解析文本，两端空白会被忽略
         * @return std::optional<std::uint64_t> 解析结果；空串、负数或带尾随字符时为 nullopt
         */
        std::optional<std::uint64_t> parseUnsignedDecimal(const std::string_view text) noexcept
        {
            const std::string_view valueText = trimmedAsciiWhitespace(text);
            if (valueText.empty())
            {
                return std::nullopt;
            }

            std::uint64_t                value  = 0;
            const std::from_chars_result parsed = std::from_chars(valueText.data(), valueText.data() + valueText.size(), value);
            // 尾随字符也算失败：只解析出前半段的数字不能当成这个文件的读数
            if (parsed.ec != std::errc{} || parsed.ptr != valueText.data() + valueText.size())
            {
                return std::nullopt;
            }
            return value;
        }

        /**
         * @brief 解析限额文件：max 与 v1 内存哨兵都折成 0（明确无限制）
         * @param text 文件内容
         * @param hasLegacyUnlimitedSentinel 是否是 v1 的 memory.limit_in_bytes
         * @return std::optional<std::uint64_t> 限额字节数或进程数；0 = 无限制，nullopt = 读不出
         */
        std::optional<std::uint64_t> parseLimitValue(const std::string_view text, const bool hasLegacyUnlimitedSentinel) noexcept
        {
            if (trimmedAsciiWhitespace(text) == "max")
            {
                return std::uint64_t{0};
            }

            const std::optional<std::uint64_t> value = parseUnsignedDecimal(text);
            if (!value.has_value())
            {
                return std::nullopt;
            }
            // v1 的哨兵不是真实字节数，按无限制报；它远大于任何合理容器上限，用下限判定即可
            if (hasLegacyUnlimitedSentinel && *value >= kLegacyUnlimitedMemoryBytes)
            {
                return std::uint64_t{0};
            }
            return value;
        }

        /**
         * @brief 把一行 v1 控制器列表里命中的 memory/pids 记进位置表
         * @param controllers 冒号之间的控制器列表，逗号分隔
         * @param relativePath 该行的控制组路径
         * @param[out] layout 待填的位置表
         */
        void recordVersionOneControllers(const std::string_view controllers, const std::string_view relativePath, CgroupLayout &layout)
        {
            std::string_view remainingControllers = controllers;
            while (true)
            {
                const std::size_t      separator  = remainingControllers.find(',');
                const std::string_view controller = trimmedAsciiWhitespace(remainingControllers.substr(0, separator));
                if (controller == "memory" && !layout.memory.has_value())
                {
                    layout.memory = ControllerLocation{.isUnified = false, .relativePath = std::string(relativePath)};
                } else if (controller == "pids" && !layout.pids.has_value())
                {
                    layout.pids = ControllerLocation{.isUnified = false, .relativePath = std::string(relativePath)};
                }

                if (separator == std::string_view::npos)
                {
                    return;
                }
                remainingControllers.remove_prefix(separator + 1);
            }
        }

        /**
         * @brief 解析 /proc/self/cgroup：v1 控制器行优先，未出现的控制器落到 v2 的 0:: 行
         * @param contents /proc/self/cgroup 的全文
         * @return CgroupLayout 两个控制器的位置
         */
        CgroupLayout parseCgroupLayout(const std::string_view contents)
        {
            CgroupLayout layout;
            for (std::size_t lineStart = 0; lineStart < contents.size();)
            {
                const std::size_t      lineEnd     = contents.find('\n', lineStart);
                const std::size_t      lineSize    = (lineEnd == std::string_view::npos ? contents.size() : lineEnd) - lineStart;
                const std::string_view line        = contents.substr(lineStart, lineSize);
                const std::size_t      firstColon  = line.find(':');
                const std::size_t      secondColon = firstColon == std::string_view::npos ? std::string_view::npos : line.find(':', firstColon + 1);
                if (secondColon != std::string_view::npos)
                {
                    const std::string_view hierarchyId  = trimmedAsciiWhitespace(line.substr(0, firstColon));
                    const std::string_view controllers  = line.substr(firstColon + 1, secondColon - firstColon - 1);
                    const std::string_view relativePath = trimmedAsciiWhitespace(line.substr(secondColon + 1));
                    if (!relativePath.empty())
                    {
                        if (controllers.empty())
                        {
                            // v2 统一层级的层级号固定为 0；其它空控制器行不是这条通路的形状
                            if (hierarchyId == "0" && !layout.hasUnifiedHierarchy)
                            {
                                layout.hasUnifiedHierarchy = true;
                                layout.unifiedRelativePath = std::string(relativePath);
                            }
                        } else
                        {
                            recordVersionOneControllers(controllers, relativePath, layout);
                        }
                    }
                }

                if (lineEnd == std::string_view::npos)
                {
                    break;
                }
                lineStart = lineEnd + 1;
            }

            // 混合层级里 memory 与 pids 可能分属不同版本：只在各自没有 v1 记录时才用统一层级
            if (layout.hasUnifiedHierarchy)
            {
                if (!layout.memory.has_value())
                {
                    layout.memory = ControllerLocation{.isUnified = true, .relativePath = layout.unifiedRelativePath};
                }
                if (!layout.pids.has_value())
                {
                    layout.pids = ControllerLocation{.isUnified = true, .relativePath = layout.unifiedRelativePath};
                }
            }
            return layout;
        }

        /**
         * @brief 拼出控制器在给定挂载根下的目录
         * @details 相对路径以 '/' 开头，直接拼到 path 上会把它替换成绝对路径，因此先剥掉前导斜杠。
         * @param cgroupRoot 挂载根
         * @param isUnified 是否 v2 统一层级
         * @param controllerName v1 的控制器目录名（memory/pids）；v2 不使用
         * @param relativePath /proc/self/cgroup 给出的控制组路径
         * @return std::filesystem::path 控制器目录
         */
        std::filesystem::path controllerDirectory(const std::filesystem::path &cgroupRoot, const bool isUnified, const std::string_view controllerName,
                                                  const std::string &relativePath)
        {
            std::filesystem::path directory = cgroupRoot;
            if (!isUnified)
            {
                directory /= std::filesystem::path(std::string(controllerName));
            }

            std::string_view relative = relativePath;
            while (!relative.empty() && relative.front() == '/')
            {
                relative.remove_prefix(1);
            }
            if (!relative.empty())
            {
                directory /= std::filesystem::path(std::string(relative));
            }
            return directory;
        }
    } // namespace

    CgroupLimits CgroupInfo::read() noexcept
    {
#if ASYN_PLATFORM_LINUX
        const std::expected<std::string, std::error_code> contents = readFileContents("/proc/self/cgroup", 0, kSelfCgroupProbeLength);
        if (!contents.has_value() || contents->empty() || contents->size() >= kSelfCgroupProbeLength)
        {
            return {};
        }
        return readFrom(std::filesystem::path{"/sys/fs/cgroup"}, *contents);
#else
        // Windows 没有 cgroup：返回空读数，让调用方按「本环境读不出」处理
        return {};
#endif
    }

    CgroupLimits CgroupInfo::readFrom(const std::filesystem::path &cgroupRoot, const std::string_view selfCgroupContents) noexcept
    {
        const CgroupLayout layout = parseCgroupLayout(selfCgroupContents);
        CgroupLimits       limits;

        if (layout.memory.has_value())
        {
            const std::filesystem::path directory       = controllerDirectory(cgroupRoot, layout.memory->isUnified, "memory", layout.memory->relativePath);
            const char *const           limitFileName   = layout.memory->isUnified ? "memory.max" : "memory.limit_in_bytes";
            const char *const           currentFileName = layout.memory->isUnified ? "memory.current" : "memory.usage_in_bytes";
            if (const std::optional<std::string> text = readCgroupValueFile(directory / limitFileName); text.has_value())
            {
                limits.memoryLimitBytes = parseLimitValue(*text, !layout.memory->isUnified);
            }
            if (const std::optional<std::string> text = readCgroupValueFile(directory / currentFileName); text.has_value())
            {
                limits.memoryCurrentBytes = parseUnsignedDecimal(*text);
            }
        }

        if (layout.pids.has_value())
        {
            // 两个版本的 pids 读数文件名相同，目录层级才是差异所在
            const std::filesystem::path directory = controllerDirectory(cgroupRoot, layout.pids->isUnified, "pids", layout.pids->relativePath);
            if (const std::optional<std::string> text = readCgroupValueFile(directory / "pids.max"); text.has_value())
            {
                // v1 的 pids.max 也写 max，不应套用内存那个哨兵判定
                limits.pidsLimit = parseLimitValue(*text, false);
            }
            if (const std::optional<std::string> text = readCgroupValueFile(directory / "pids.current"); text.has_value())
            {
                limits.pidsCurrent = parseUnsignedDecimal(*text);
            }
        }

        return limits;
    }
} // namespace AsynGyanis::Platform
