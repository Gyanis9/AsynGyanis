/**
 * @file YamlReader.h
 * @brief YAML 流式事件读取器：push 喂入 / pull 取事件，与 DOM 版共用同一扫描器
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Format/Yaml/YamlEvent.h"
#include "Base/Format/Yaml/YamlParseOptions.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Base
{
    class YamlReaderImplementation;

    /**
     * @brief YAML 流式事件读取器
     *
     * @details 与 DOM 版 YamlParser 的关系：两者是**同一份扫描器之上的两个前端**。
     *          YamlReader 完成全部词法与块结构识别（缩进、块标量、引号跨行、流式跨行、
     *          指令、标签、锚点），并以 YamlEvent 序列暴露结果；YamlParser 只消费这些事件
     *          组装 FormatValue。因此不存在「两套解析逻辑各自漂移」的问题，
     *          新增语法特性只需改动扫描器一处，两个前端同时受益。
     *
     *          事件在首次取用（hasNext/nextEvent）时一次性生成并缓存，调用方按顺序取用；
     *          这样既保证块结构所需的完整前瞻，又让事件数量只与文档规模成正比（不构建 DOM）。
     *
     *          两种输入方式：
     *          - 构造时传入完整文本：零拷贝，读取器仅持有 string_view，调用方需保证
     *            文本生命周期覆盖整个读取过程；
     *          - 默认构造后反复 feed()：分片追加到内部缓冲，全部喂完后必须调用 finish()
     *            声明输入结束；此时输入由读取器所有。
     */
    class YamlReader
    {
    public:
        /**
         * @brief 构造空读取器（push 模式）
         * @details 通过 feed() 逐片喂入输入，喂完后调用 finish()。
         */
        YamlReader();

        /**
         * @brief 以完整文本构造读取器（零拷贝 pull 模式）
         * @param text UTF-8 编码的 YAML 文本，生命周期须覆盖读取全过程
         * @param options 解析选项
         */
        explicit YamlReader(std::string_view text, const YamlParseOptions &options = YamlParseOptions{});

        /**
         * @brief 析构函数
         * @details 在实现文件中定义，以隐藏 pimpl 实现类型的完整定义。
         */
        ~YamlReader();

        /**
         * @brief 移动构造函数
         * @details 转移内部实现对象；源对象退化为可析构但不可再用。
         * @param other 待移源的读取器
         */
        YamlReader(YamlReader &&other) noexcept;

        /**
         * @brief 移动赋值运算符
         * @details 释放自身实现对象后接管源对象；自移动安全。
         * @param other 待移源的读取器
         * @return YamlReader& 本对象引用
         */
        YamlReader &operator=(YamlReader &&other) noexcept;

        // 读取器持有输入缓冲与扫描状态，拷贝会产生两份互不相干的游标，语义不清晰，故禁止
        YamlReader(const YamlReader &) = delete;
        YamlReader &operator=(const YamlReader &) = delete;

        /**
         * @brief push 模式喂入一段输入
         * @details 分片追加到内部缓冲；仅在未调用 finish() 前可继续喂入。
         *          一旦开始取事件（hasNext/nextEvent）便不得再喂入。
         * @param chunk 待追加的文本片段
         * @throws FormatError 已开始取事件后仍尝试喂入
         */
        void feed(std::string_view chunk);

        /**
         * @brief 声明输入结束并触发事件生成
         * @details 幂等；未调用时 nextEvent() 返回空。
         * @throws FormatError 输入超出 maximumInputLength
         */
        void finish();

        /**
         * @brief 判断是否还有未取用的事件
         * @details 首次调用会触发扫描（必要时先补上隐式 finish）。
         * @return true 尚有事件可取
         */
        [[nodiscard]] bool hasNext();

        /**
         * @brief 取出下一条事件
         * @return std::optional<YamlEvent> 下一条事件；事件耗尽后返回空
         * @throws FormatError 扫描期发现语法错误（错误的 kind 与行列已写入异常）
         */
        [[nodiscard]] std::optional<YamlEvent> nextEvent();

        /**
         * @brief 一次性读取全部事件
         * @details 便捷入口，适合小文档或需要随机访问事件流的场景；
         *          大文档建议直接用 nextEvent() 逐条消费。
         * @param text UTF-8 编码的 YAML 文本
         * @param options 解析选项
         * @return std::vector<YamlEvent> 完整事件序列（含 StreamStart/StreamEnd）
         * @throws FormatError 语法非法或超出选项上限
         */
        [[nodiscard]] static std::vector<YamlEvent> readAll(std::string_view text, const YamlParseOptions &options = YamlParseOptions{});

        /**
         * @brief 获取解析选项快照
         * @return const YamlParseOptions& 构造时确定的选项
         */
        [[nodiscard]] const YamlParseOptions &options() const noexcept;

        /**
         * @brief 获取解析期收集的非致命警告
         * @details 目前仅记录「保留指令被忽略」（YAML 1.2 §6.8 允许处理器忽略无法识别的指令，
         *          其参数个数不受约束）与「未声明的 %TAG 句柄」两类情形；rejectUnknownDirectives
         *          为真时改为直接报错，不产生警告。首次调用会触发扫描（扫描结果幂等）。
         * @return std::vector<std::string>& 警告文本列表，按出现顺序
         * @throws FormatError 扫描期发现语法错误
         */
        std::vector<std::string> &warnings();

    private:
        /**
         * @brief 触发一次扫描并缓存事件（幂等）
         * @throws FormatError 语法非法或超出选项上限
         */
        void ensureScanned();

        std::unique_ptr<YamlReaderImplementation> m_implementation; ///< 扫描器实现（含输入缓冲与事件缓存）
    };
} // namespace AsynGyanis::Base
