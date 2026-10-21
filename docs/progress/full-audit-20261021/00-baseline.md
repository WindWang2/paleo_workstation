# 基线（full-audit-20261021）

- 日期：2026-10-21
- BASE_SHA：`e6837e143ce59ce625306754eb3dd98c79aeb84b`（origin/master 缓存引用，#598 Windows 便携化收口）
- 分支：`codex/full-review-fix-review-fix-20261021`
- Worktree：`/home/kevin/projects/paleo_workstation-full-review-20261021`（独立目录，不共享 build）
- 平台：linux x64（cachyos-lts 6.18.55-2），zsh

## 网络状态（开工时）

GitHub HTTPS 直连与本地代理（127.0.0.1:7890，http/socks5 均）TLS EOF 不可达；
`git fetch` 与 `gh` 均失败。BASE_SHA 取本地缓存的 origin/master（上次成功 fetch
已含 #598）。后台已启动周期重试；后续 push/PR 前必须重新 fetch 对账（见 13 节纪律）。

## 外部活跃工作（三支未合并分支，均基于/含 master 附近代码）

| 分支 | 提交数 | 触碰文件 | 内容 |
|---|---|---|---|
| codex/full-review-fix-review-fix-20261019（PR #580） | 17 | 51 | 上上轮综合整顿 round 3 |
| codex/full-review-fix-review-fix-20261020（PR 应已开） | 23 | 54 | 上轮综合整顿 round 4 |
| codex/fix-review-20261019（PR 应已开） | 27 | 226 | review-from-zero 报告 147 项发现的批次修复（#582–#597 族） |

三支并集 295 文件（src/ 内 189/1194）。约 1005 个 src 文件未被触碰。

## 上轮（20261020）登记未修项（本轮池候选，无 issue/PR 占有）

- D-06 correlationpanel.cpp:118-152 m_dragColumn 裸指针 UAF（latent）
- D-09 layoutexportactions.cpp:397-401 任务启动失败双重上报
- E-14 check_i18n.py 入口词表盲区（addItem/insertItem/setTabText 等）
- A-12 mapversionstore.cpp:324-390 冻结目录在事务外（journal 化需独立批次——本轮不接）
- A-16 frameworkstore.cpp:259-265 flush 失败不清孤儿字节
- C-12 paleomapsource worker 内建 QgsLayer（契约注释建议）
- D-14 杂项 5 条（petrophys outputUnit / 轨道 caption 叠压 / horizonmarkers 空名 / mapbook 双份日志 / 深度道 first() 兜底）

## 构建基线

（待建立独立 build 后回填：配置、Qt/QGIS/GDAL 版本、编译结果、测试基线）
