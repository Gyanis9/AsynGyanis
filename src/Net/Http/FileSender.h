/**
 * @file FileSender.h
 * @brief 静态文件辅助工具：按扩展名推断 HTTP Content-Type
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <string>

namespace AsynGyanis::Net
{
    /**
     * @brief 静态文件辅助工具类，只提供无状态的扩展名到 MIME 映射查询。
     *
     * @details 本类不持有任何状态，因此被设计为不可实例化：构造函数与拷贝/移动一律私有且
     *          delete，调用方只能以 FileSender::contentTypeForFile() 的形式使用。
     *          之所以写成类而不是命名空间里的自由函数：现有调用方（HttpServer 的静态路由）
     *          已按「类名限定」的形式书写，改成自由函数会连带改动其他模块负责的代码。
     * @note 历史上这里还提供一个基于 sendfile(2) / TransmitFile 的零拷贝 sendFile()。
     *       它在全仓库没有任何消费者，且 Windows 分支把 TransmitFile 当成「按块递减剩余长度」
     *       的循环来用（TransmitFile 自己会推进文件指针，且返回后偏移语义不受调用方控制），
     *       错误码又取自 errno 而非 WSAGetLastError，属于双重缺陷，已整体删除。
     *       若以后确实需要零拷贝发送，应先在 Platform 层补出跨平台的 sendfile 封装再实现。
     */
    class FileSender
    {
    public:
        /**
         * @brief 按文件路径推断 HTTP Content-Type
         * @details 只取最后一段扩展名查表，扩展名比较不区分大小写（HTTP 惯例：
         *          .HTML 与 .html 必须落到同一个 MIME，否则站点文件改名大小写后会掉进
         *          application/octet-stream 而无法内联预览）。表内没有的类型一律按
         *          二进制流下发，由浏览器自行决定是下载还是预览。
         * @param filePath 文件路径或文件名，仅其扩展名参与判定
         * @return const char* 指向静态存储的 MIME 文本，调用方可长期持有、无需释放
         */
        static const char *contentTypeForFile(const std::string &filePath);

    private:
        // 纯静态工具类：把构造与拷贝/移动全部私有化并 delete，任何实例化写法都会在编译期失败
        FileSender() = delete;
        FileSender(const FileSender &) = delete;
        FileSender &operator=(const FileSender &) = delete;
        FileSender(FileSender &&) = delete;
        FileSender &operator=(FileSender &&) = delete;
    };
} // namespace AsynGyanis::Net
