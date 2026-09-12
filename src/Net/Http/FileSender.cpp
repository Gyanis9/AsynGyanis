#include "Net/Http/FileSender.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 未识别扩展名时下发的媒体类型
         *
         * @details 按二进制流处理是最安全的兜底：浏览器不会把未知内容当脚本文本执行，
         *          只会退化成下载，宁缺勿错。
         */
        constexpr const char *kUnknownMimeType = "application/octet-stream";
    } // namespace

    const char *FileSender::contentTypeForFile(const std::string &filePath)
    {
        // 表内键一律小写，查询方负责把待比较文本转小写。
        // 键类型用 string_view：字符串字面量的内容与时序都是静态的，查表时不必再构造临时 string
        static const std::unordered_map<std::string_view, const char *> kMimeTypesByExtension = {
                {".html", "text/html"},
                {".htm",  "text/html"},
                {".css",  "text/css"},
                {".js",   "application/javascript"},
                {".mjs",  "application/javascript"},
                {".json", "application/json"},
                {".xml",  "application/xml"},
                {".txt",  "text/plain"},
                {".pdf",  "application/pdf"},
                {".png",  "image/png"},
                {".jpg",  "image/jpeg"},
                {".jpeg", "image/jpeg"},
                {".gif",  "image/gif"},
                {".svg",  "image/svg+xml"},
                {".ico",  "image/x-icon"},
                {".webp", "image/webp"},
                {".woff", "font/woff"},
                {".woff2", "font/woff2"},
                {".ttf",  "font/ttf"},
                {".wasm", "application/wasm"},
        };

        // 只看最后一段扩展名：bundle.min.js 按 .js 判定；没有点的路径无扩展名可言
        const std::size_t dotPosition = filePath.rfind('.');
        if (dotPosition == std::string::npos)
        {
            return kUnknownMimeType;
        }

        // 连点一起截取，与表内键的形式保持一致。HTTP 的扩展名大小写不敏感，
        // 因此必须先归一化成小写再查表，否则 IMG.JPG 会落到兜底的二进制流
        std::string lowerCaseExtension(filePath.begin() + static_cast<std::ptrdiff_t>(dotPosition), filePath.end());
        std::ranges::transform(lowerCaseExtension, lowerCaseExtension.begin(),
                               [](const unsigned char character)
                               {
                                   // std::tolower 只接受 unsigned char 或 EOF：
                                   // 直接传可能为负的 char 是未定义行为，故入参按 unsigned char 收
                                   return static_cast<char>(std::tolower(character));
                               });

        if (const auto iterator = kMimeTypesByExtension.find(std::string_view(lowerCaseExtension)); iterator != kMimeTypesByExtension.end())
        {
            return iterator->second;
        }

        return kUnknownMimeType;
    }

} // namespace AsynGyanis::Net
