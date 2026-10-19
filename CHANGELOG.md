# 更新日志

本文件记录每个已发布版本对使用者可见的变化。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本 2.0.0](https://semver.org/lang/zh-CN/)。

## 版本号策略

- **单一真值来源**：版本号只写在根 `CMakeLists.txt` 的 `project(AsynGyanis VERSION x.y.z)` 里，
  对外导出的 `find_package(AsynGyanis x.y)` 与安装包版本都取自它，不另外维护一份。
- **标签格式**：发布时在本文件与 CMake 版本号一致的提交上打附注标签 `v<主>.<次>.<修订>`（例如 `v1.0.0`）。
- **版本号怎么涨**：由约定式提交推导——`feat` 涨次版本，`fix` 涨修订号，正文含 `BREAKING CHANGE`
  或类型后带 `!` 涨主版本；其余类型（`refactor`/`perf`/`test`/`docs`/`chore`/`build`/`ci`）不单独抬版本。
- **一致性由脚本把关**：`scripts/check-release-version.py` 比对「CMake 版本号 / 本文件最新发布段 / 最新标签」
  三者，不一致即退出码非 0；发布门禁（`release-gate.yml`）已接入这一步（它不被路径过滤跳过），避免出现「打了标签但版本号没改」这类漂移。
- **本文件从 2026-10-19 起重记**：此前的 1.0.0～4.0.0 各段连同远端标签与 Release 一并作废并从本文件删净，
  那一段演进留在 `git log` 的发布提交里。所以下面第一个 `## [x.y.z] - 日期` 就是新线的头一笔；在它写出来之前，
  本文件没有任何条目可读，`check-release-version.py` 会按「读不到发布段」退 2——那是它在说实话，不是脚本坏了。
  另记一条给下一次发布：远端已无标签，但这台机器上仍留着那 13 个旧附注标签作谱系，`check-release-version.py`
  读的是本地 `git tag`——新线第一个标签打上之前，标签那一项会拿旧线的最高号来比，那条红是残留而不是漂移。
