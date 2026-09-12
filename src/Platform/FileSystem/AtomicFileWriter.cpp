#include "Platform/FileSystem/AtomicFileWriter.h"

#include <fstream>

namespace AsynGyanis::Platform
{
    bool AtomicFileWriter::writeText(const std::filesystem::path &targetPath, const std::string &text, const std::optional<std::filesystem::perms> permissions, std::string *error)
    {
        const auto reportFailure = [error](const std::string &reason)
        {
            if (error != nullptr)
            {
                *error = reason;
            }
            return false;
        };

        std::error_code fileSystemError;
        if (const auto parentDirectory = targetPath.parent_path(); !parentDirectory.empty() && !std::filesystem::exists(parentDirectory, fileSystemError))
        {
            std::filesystem::create_directories(parentDirectory, fileSystemError);
            if (fileSystemError)
            {
                return reportFailure("创建目录 '" + parentDirectory.string() + "' 失败：" + fileSystemError.message());
            }
        }

        const std::filesystem::path temporaryPath = targetPath.string() + ".tmp";

        {
            std::ofstream temporaryFile(temporaryPath, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!temporaryFile.is_open())
            {
                return reportFailure("无法打开临时文件 '" + temporaryPath.string() + "'");
            }

            temporaryFile.write(text.data(), static_cast<std::streamsize>(text.size()));
            temporaryFile.flush();
            if (!temporaryFile.good())
            {
                return reportFailure("写入临时文件 '" + temporaryPath.string() + "' 失败");
            }
        }

        if (permissions.has_value())
        {
            std::filesystem::permissions(temporaryPath, *permissions, std::filesystem::perm_options::replace, fileSystemError);
            if (fileSystemError)
            {
                std::filesystem::remove(temporaryPath, fileSystemError);
                return reportFailure("设置 '" + temporaryPath.string() + "' 权限失败：" + fileSystemError.message());
            }
        }

        std::filesystem::rename(temporaryPath, targetPath, fileSystemError);
        if (fileSystemError)
        {
            std::filesystem::remove(temporaryPath, fileSystemError);
            return reportFailure("替换 '" + targetPath.string() + "' 失败：" + fileSystemError.message());
        }

        return true;
    }
} // namespace AsynGyanis::Platform
