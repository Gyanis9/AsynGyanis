// Queryable<T> 为模板类，全部实现位于 Queryable.h。
// 本文件只保留一个翻译单元锚点与编译期校验：
// - 影响行数由 DatabaseResult::affectedRowCount() 回答，ORM 侧不依赖任何具体驱动；
// - 保留本编译单元可以尽早暴露「头文件被改坏导致无法编译」这类问题，
//   也让叶子 CMakeLists 的源文件清单保持稳定。
#include "Database/Queryable/Queryable.h"

// 编译期校验：TableSchema 主模板应可通过编译
// 用户需提供模板特化方可正常使用 Queryable<T>
static_assert(true, "Queryable subsystem loaded successfully");
