# 可选可信模板的离线供货

这套工具只供开发者或操作者手动使用，不是 Agent 环境安装入口，不执行模板、
不运行 pip、不联网，也不改变默认安装结果。默认应用安装仍不包含 `.py`。
运行时供应继续使用独立的 `prepare_agent_python.py`；模板使用独立 scripts store。

输入仅为仓库 `vendor/agent-python/templates/` 中的三个固定普通文件：
`summary.py`、`well_quality.py`、`attribute_correlation.py`。目录中缺项、多项、
链接、reparse point、硬链接、非 UTF-8/LF、语法损坏或单份超过 256 KiB 均拒绝。
语法检查仅构建 AST，不执行代码。代码按原始字节保存，与宿主编译 SHA 一致。

当前根 `NOTICE.md` 明确说明本仓库自身 LICENSE 尚未定稿。包中保留该真实说明，
`licenseStatus=notice-only`，不编造 SPDX 名称或授予再分发权。若仓库实际存在根
LICENSE/COPYING 文本，则一并收录并标记 `repository-texts-included`，仍不推断
许可名称。notice/licence 文本仅将行尾规范为 UTF-8/LF，正文保留；不把 NumPy
或 CPython 的许可误认为这三份仓库模板的许可。

## 手动流程

在与宿主构建对应的 checkout 中运行（下面 SHA 是占位符，应使用供货输出及
独立交付记录中的实际值）：

```text
python tools/check_agent_python_scripts.py
python tools/check_agent_python_scripts.py --selftest
python tools/prepare_agent_python_scripts.py --bundle /operator/offline/python-scripts.zip
python tools/check_agent_python_scripts.py --archive /operator/offline/python-scripts.zip --manifest-sha256 <manifestSHA> --source-root <checkout>
python tools/prepare_agent_python_scripts.py --archive /operator/offline/python-scripts.zip --manifest-sha256 <manifestSHA> --store /operator/scripts-store
python tools/check_agent_python_scripts.py --generation /operator/scripts-store/generations/<manifestSHA> --manifest-sha256 <manifestSHA>
```

打包输出提供 ZIP SHA、manifest SHA、source runtime lock SHA 和三份脚本 SHA。
`--manifest-sha256` 必须由操作者从独立可信交付记录提供，不从包内自报字段推导。
同一组字节生成可重复 ZIP；只接受本工具的无压缩、固定元数据、扁平 ZIP 格式。
目录、路径穿越、额外/重复/大小写别名、链接和非普通文件、额外 ZIP 字段/注释、
前后缀垃圾、损坏内容、超预算或非规范 manifest 均拒绝。不调用 extractall。
manifest 最多 64 KiB，文件总量最多 3 MiB，归档最多 3 MiB + 128 KiB。

## Generation 与部署

准备工具在 OS 排他锁内写自有 staging，逐文件校验并设只读后，才发布
`generations/<manifestSHA>`。已存在 generation 只验证，不原地改写或删除。
原子替换 `current.json` 是激活提交点：此前的异常或中断不改变旧 pointer；
提交后 pointer 只可能指向已完成校验的 generation。强制终止可能遗留未选中的
staging/current 临时文件或完整 generation，绝不会据此自动激活；不自动 GC。
普通异常会清理本次自有 staging，旧 generation 始终保留。

操作者显式将宿主环境变量 `PALEO_AGENT_PYTHON_SCRIPTS_ROOT` 指向完整 generation
目录，例如 `/operator/scripts-store/generations/<manifestSHA>`，不指向 store、
ZIP 或 current.json。工具不修改该环境变量、不启动或重启应用。`current.json`
是操作者选择记录，不是宿主脚本授权来源。

宿主在 freeze 时必须把所选脚本的 SHA 与编译进 C++ 的对应固定锚比较，
再将认证字节写入持久 InputBundle 的 code.py。manifest 自报、包校验通过或
current.json 更新都不能授予代码执行权限。已冻结操作自带 code.py 与其 SHA，
无需长期 purge lease；所有 generations 可一直保留，无自动清理旧版本。
模板修改、对应编译锚更新、编译测试和源码门禁必须在同一评审批次完成。

## Manifest 与门禁

`scripts-manifest.json` 使用规范 UTF-8 JSON（字段排序、末尾 LF），结构示意：

```json
{
  "schema": "paleo.python-scripts.v1",
  "version": 1,
  "profile": "analysis-core",
  "runtimeVersion": "<runtime.lock.json value>",
  "pythonVersion": "<runtime.lock.json value>",
  "numpyVersion": "<runtime.lock.json value>",
  "sourceRuntimeLockSha256": "<SHA of original runtime.lock.json bytes>",
  "scripts": [{"id":"summary","path":"summary.py","source":"vendor/agent-python/templates/summary.py","size":123,"sha256":"<SHA>"}],
  "licenses": [{"path":"NOTICE.md","source":"NOTICE.md","kind":"notice","size":123,"sha256":"<SHA>"}],
  "licenseStatus": "notice-only",
  "treeSize": 123
}
```

实际 scripts 必须恰好包含按 id 排序的三项；licences 按 path 排序。版本 1
表示供货格式，脚本内容版本由各自 SHA 唯一标识。runtime lock SHA 保留来源联系，
运行时二进制及其第三方许可不进入此 ZIP。

新增源码门禁命令为 `python tools/check_agent_python_scripts.py` 和
`python tools/check_agent_python_scripts.py --selftest`。自测只使用临时目录和
本地标准库，覆盖恶意归档、manifest、只读代际、并发锁、写入/提交中断及进程死亡；
不会运行任何供应模板或下载依赖。preparer 的 `--selftest` 运行同一组测试。
