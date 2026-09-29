/**
 * @file FileSpanExporter.h
 * @brief 把链路节按「一行一条 JSON」写进文件的出口
 * @author Gyanis
 * @date 2026-09-27
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Net/Tracing/SpanExporter.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief 文件链路出口：每条节一行 JSON（JSON Lines）
     *
     * @details 与 OTLP 出口互补的那一半：不依赖任何采集端就能 `jq`/`rg` 查现场，也不会在
     *          Collector 失联时把整批丢掉。落盘的形状是自描述的——出处（服务名/版本）写在每一行里，
     *          于是任意切一段文件出来都能单独读。
     * @note 行尾固定 "\n"，Windows 上也不翻成 "\r\n"：这份文件是给工具逐行解析的，
     *       JSON Lines 的分行符就是 LF，混进 CR 会让按字节切行的读取器多出一个不可见字段。
     * @note 一律以追加方式打开，且自动创建父目录：进程重启不该把上一批链路清掉。
     * @see formatOtlpTracesJson(), Tracer
     */
    class ASYN_NET_API FileSpanExporter final : public SpanExporter
    {
    public:
        /**
         * @brief 打开（必要时创建）目标文件
         * @param filePath 目标文件路径；父目录不存在时一并建出来
         * @throws Base::SystemException 父目录建不出来，或文件打不开
         */
        explicit FileSpanExporter(std::filesystem::path filePath);

        /**
         * @brief 析构：关闭文件（缓冲内容由 ofstream 的析构负责落盘）
         */
        ~FileSpanExporter() override;

        FileSpanExporter(const FileSpanExporter &)            = delete;
        FileSpanExporter &operator=(const FileSpanExporter &) = delete;
        FileSpanExporter(FileSpanExporter &&)                 = delete;
        FileSpanExporter &operator=(FileSpanExporter &&)      = delete;

        /**
         * @brief 把一批节逐行写进文件，随后刷新到盘
         * @details 重写 SpanExporter::exportSpans()：一次写入一批、末尾一次 flush——批量的意义就在这里，
         *          每条一节各刷一次盘会让出口线程整个排在磁盘后面。
         * @param resource 出处，写进每一行的 serviceName/serviceVersion 两个字段
         * @param spans 待写出的节
         * @return true 整批都已写出
         * @return false 中途有行没能写出：已写出的行不会撤回，但契约上没有「部分收下」这一档，
         *         整批判为没收，让丢失规模在 Tracer 的计数里看得见
         * @note 有一行的正文没能成形（取值进不了 JSON）时也只算整批没收：本出口不猜是哪条坏了，
         *       也不静默跳过——跳过等于把「链路缺一段」伪装成「本来就没有这一段」
         */
        bool exportSpans(const TraceResource &resource, const std::vector<SpanRecord> &spans) override;

        /**
         * @brief 出口名固定为 file
         * @details 重写 SpanExporter::exporterName()：计数与告警文本里靠它认路。
         * @return std::string_view "file"
         */
        [[nodiscard]] std::string_view exporterName() const noexcept override;

        /**
         * @brief 刷新并关闭文件
         * @details 重写 SpanExporter::shutdown()：Tracer 停止时最后一次交付之后调用一次，
         *          把 ofstream 缓冲里的尾巴落到盘上。不抛异常。
         */
        void shutdown() noexcept override;

        /// @brief 目标文件路径
        [[nodiscard]] const std::filesystem::path &filePath() const noexcept;

    private:
        std::filesystem::path m_filePath;   ///< 目标文件路径
        mutable std::mutex    m_writeMutex; ///< 护住写出与关闭：出口线程与 flush() 可能来自不同线程
        std::ofstream         m_file;       ///< 已打开的输出流（二进制模式，行尾自己补）
    };
} // namespace AsynGyanis::Net
