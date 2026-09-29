/**
 * @file MultipartForm.h
 * @brief `multipart/form-data` 正文的解析：段序列、字段名与上传文件的表达
 * @author Gyanis
 * @date 2026-09-27
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 上传文件在框架里原本没有表达途径：`HttpRequest::formFields()` 按 '&' 切正文，对 multipart
 *          正文只会切出垃圾。本类按 RFC 2046 §5.1 的分隔符规则与 RFC 7578 的表单约定把正文切成段，
 *          段正文以视图交出、不额外拷贝一份正文。
 */

#pragma once

#include "AsynGyanisExport.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief multipart 正文里的一个段：一个表单字段或一份上传文件
     *
     * @details 只带解析器实际消费的那几条信息（name/filename/Content-Type/正文）。
     *          `content` 是指向整份请求正文的视图，本对象不持有那些字节。
     * @note 文本按对端送来的原样字节保存（通常是 UTF-8），不做百分号解码也不做字符集转换：
     *       替调用方猜编码会猜错，且猜错的结果看起来完全正常。
     */
    struct ASYN_NET_API MultipartPart
    {
        std::string      name;        ///< Content-Disposition 的 name 参数，必非空
        std::string      fileName;    ///< filename 参数；普通字段与 `filename=""` 都为空。已剥掉目录部分
        std::string      contentType; ///< 段自己的 Content-Type 原值；对端没给时为空串
        std::string_view content;     ///< 段正文，视图指向所属请求的正文

        /**
         * @brief 本段是不是上传的文件
         * @return true 带了非空 filename
         */
        [[nodiscard]] bool isFile() const noexcept
        {
            return !fileName.empty();
        }
    };

    /**
     * @brief 一份解析好的 multipart 表单：按正文里的出现顺序排列的段序列
     *
     * @details 解析规则（一次解析要么全成、要么整体判失败，不留「半份表单」）：
     *          @li 分隔符必须在行首（正文开头或紧跟一处 CRLF），其前的 preamble 与其后的 epilogue
     *              都按规范忽略（RFC 2046 §5.1）；
     *          @li 段只消费 Content-Disposition、Content-Type、Content-Transfer-Encoding 三条，
     *              其余（Content-ID 一类元数据）放过；
     *          @li 处置类型必须是 `form-data`，name 必须存在且非空，同一段的上述三条头部各只能出现一次；
     *          @li 边界按 RFC 2046 §5.1.1 的 bchars 校验（至多 70 字符、不以空格收尾）；
     *          @li 行尾只认 CRLF，正文里不允许 obs-fold（续行）。
     * @note **不支持的写法一律拒绝而不是将就**：`filename*`/`name*`（RFC 2231 扩展参数）与
     *       `Content-Transfer-Encoding: base64`、`quoted-printable` 都会让整份表单判失败。后两者需要
     *       把段正文换成解码后的新字节序列，与本类「段正文是请求正文的视图」这一零拷贝口径冲突；
     *       宁可让调用方看见失败，也不把没还原的编码文本当原始字节交出去。
     * @note `filename` 取自对端，不可信：本类先按 RFC 9110 §5.6.4 展开引号里的转义，再剥掉目录部分
     *       （留最后一段），并拒绝含控制字符的名字、拒绝收成 "." 或 ".." 的名字。老 Windows 客户端
     *       那种「反斜杠没转义」的路径经转义展开后只剩一个怪名字，目录分隔符不会留下，因此拼不出穿越
     *       路径。落盘时仍须自己决定目录、并对重名做处理，不要直接拼接这个值。
     * @see HttpRequest::multipartForm()
     */
    class ASYN_NET_API MultipartFormData
    {
    public:
        /**
         * @brief 解析一份 multipart 正文
         * @param body 正文全部字节（已收齐的整份，不是流）
         * @param contentType `Content-Type` 头部原值，边界写在它的参数里
         * @return std::optional<MultipartFormData> 解析结果；媒体类型不是 multipart/form-data、
         *         边界缺失或畸形、正文结构不合规范时为空
         */
        [[nodiscard]] static std::optional<MultipartFormData> parse(std::string_view body, std::string_view contentType);

        /**
         * @brief 取全部段
         * @return 按正文出现顺序排列的段序列；同名的字段与文件都保留
         */
        [[nodiscard]] const std::vector<MultipartPart> &parts() const noexcept;

        /**
         * @brief 取第一个指定 name 的段
         * @param name 段名，区分大小写（对端写的是什么就是什么，不做归一化）
         * @return 指向内部段序列的指针，生命周期跟随本对象；没有该段时为空指针
         */
        [[nodiscard]] const MultipartPart *findPart(std::string_view name) const noexcept;

        /**
         * @brief 取指定 name 的第一个**字段**（非文件段）的正文
         * @details 常规模板里「读一个文本框」就要这一条：文件段的正文是原始字节，按文本用会读出乱码，
         *          所以这里只认非文件段。
         * @param name 字段名
         * @return 字段取值视图；该 name 没有非文件段时为空
         */
        [[nodiscard]] std::optional<std::string_view> fieldValue(std::string_view name) const;

    private:
        /// 只由 parse() 造：没有可写入口，构造不出「半成品表单」
        explicit MultipartFormData(std::vector<MultipartPart> &&parts) noexcept;

        std::vector<MultipartPart> m_parts; ///< 段序列，findPart/fieldValue 返回的指针与视图都指向这里
    };
} // namespace AsynGyanis::Net
