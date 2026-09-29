/**
 * @file AsynGyanisExport.h
 * @brief 五个模块的符号导出宏：静态构建（默认）全部展开为空，共享构建时按平台给导出/导入或可见性属性
 * @author Gyanis
 * @date 2026-09-29
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @par 为什么只有一份而不是每模块一份
 * 五组宏只差一个前缀，抄五遍就是留五份会各自漂移的副本（最容易漂的地方是平台判定，而它一旦
 * 在一处写错，那一个模块就会静默地不导出符号，要到链接期才炸）。
 *
 * @par 静默与共享怎么判
 * 判据是 `ASYN_<模块>_SHARED_LIB`（由 CMake 以 PUBLIC 传出去，所以使用方与本库看到同一个值），
 * 构库自身再叠一条 PRIVATE 的 `ASYN_<模块>_BUILDING_SHARED_LIB` 来区分 dllexport 与 dllimport。
 * **两个都不定义时按静态处理**，而不是按共享：绕过 CMake 直接编源文件的场合确实存在
 * （scripts/fuzz-net.sh 就是这么编四类解码器的），那种场合落到的必须是「什么都不标」这一边——
 * 把它反过来会让一次没传定义的编译得到 dllimport，Windows 上报成一片找不到符号，读起来像库坏了。
 *
 * @par 嵌套类型要自己标一次
 * 外层类上的 `__declspec(dllexport)` 只覆盖外层类自己的成员，**不会**把它的嵌套类一起导出。
 * 因此跨模块用得着的嵌套类型（如 Platform::Process::Handle）必须在自己的 `class` 上再标一次，
 * 否则外层 DLL 编得出来、用到它的那个 DLL 在链接期报 LNK2019。
 *
 * @warning 共享形态下公开接口里的 std::string / std::vector 会跨 ABI 边界：生产方与消费方必须是同一套
 *          编译器、同一份 CRT、同一套标准库配置。这是 C++ 动态库本身的约束，不靠导出宏解决；要一份能
 *          被别的工具链装载的二进制，得先把接口上的标准库类型换掉，那是另一次改造。
 */

#pragma once

#if defined(_MSC_VER)
// 导出的类里带 std::string / std::vector 这类成员时 MSVC 会逐条报 C4251/C4275「需要 dll 接口」。
// 这一对警告在 C++ 动态库里是常态而不是缺陷：标准库类型跨界的正确性由上面那条 ABI 约束兜住，
// 不是靠把每个成员再导出一遍。这里不 push/pop：本头被各公共头夹在中间引入，配不上成对的 pop，
// disable 的作用域就是本翻译单元剩下的部分，这正是想要的。
#pragma warning(disable : 4251 4275)
#endif

/// Windows 用 __declspec，其他平台用可见性属性；非 Windows 上「导入」与「导出」是同一件事
#if defined(_WIN32)
#define ASYN_INTERNAL_EXPORT_SYMBOL __declspec(dllexport)
#define ASYN_INTERNAL_IMPORT_SYMBOL __declspec(dllimport)
#else
#define ASYN_INTERNAL_EXPORT_SYMBOL __attribute__((visibility("default")))
#define ASYN_INTERNAL_IMPORT_SYMBOL __attribute__((visibility("default")))
#endif

#if defined(ASYN_PLATFORM_SHARED_LIB)
#ifdef ASYN_PLATFORM_BUILDING_SHARED_LIB
#define ASYN_PLATFORM_API ASYN_INTERNAL_EXPORT_SYMBOL ///< 构 Platform 自身：导出
#else
#define ASYN_PLATFORM_API ASYN_INTERNAL_IMPORT_SYMBOL ///< 链 Platform 的一方：导入
#endif
#else
#define ASYN_PLATFORM_API ///< Platform 的公开符号；静态形态下为空
#endif

#if defined(ASYN_BASE_SHARED_LIB)
#ifdef ASYN_BASE_BUILDING_SHARED_LIB
#define ASYN_BASE_API ASYN_INTERNAL_EXPORT_SYMBOL ///< 构 Base 自身：导出
#else
#define ASYN_BASE_API ASYN_INTERNAL_IMPORT_SYMBOL ///< 链 Base 的一方：导入
#endif
#else
#define ASYN_BASE_API ///< Base 的公开符号；静态形态下为空
#endif

#if defined(ASYN_CORE_SHARED_LIB)
#ifdef ASYN_CORE_BUILDING_SHARED_LIB
#define ASYN_CORE_API ASYN_INTERNAL_EXPORT_SYMBOL ///< 构 Core 自身：导出
#else
#define ASYN_CORE_API ASYN_INTERNAL_IMPORT_SYMBOL ///< 链 Core 的一方：导入
#endif
#else
#define ASYN_CORE_API ///< Core 的公开符号；静态形态下为空
#endif

#if defined(ASYN_NET_SHARED_LIB)
#ifdef ASYN_NET_BUILDING_SHARED_LIB
#define ASYN_NET_API ASYN_INTERNAL_EXPORT_SYMBOL ///< 构 Net 自身：导出
#else
#define ASYN_NET_API ASYN_INTERNAL_IMPORT_SYMBOL ///< 链 Net 的一方：导入
#endif
#else
#define ASYN_NET_API ///< Net 的公开符号；静态形态下为空
#endif

#if defined(ASYN_DATABASE_SHARED_LIB)
#ifdef ASYN_DATABASE_BUILDING_SHARED_LIB
#define ASYN_DATABASE_API ASYN_INTERNAL_EXPORT_SYMBOL ///< 构 Database 自身：导出
#else
#define ASYN_DATABASE_API ASYN_INTERNAL_IMPORT_SYMBOL ///< 链 Database 的一方：导入
#endif
#else
#define ASYN_DATABASE_API ///< Database 的公开符号；静态形态下为空
#endif
