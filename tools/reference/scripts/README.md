# 用户脚本契约（方向68：Python 脚本面）

本目录是**用户自定义 Python 脚本**的契约文档与示例。脚本经「Python 脚本」
控制台面板运行（主窗口底栏页签），由 `ScriptRunnerService` 以子进程方式执行。

## 安全口径（先读）

- 脚本执行恒为**显式用户动作**：应用不会自动运行任何脚本，也不内置任何
  「下载并执行」通道。
- **没有沙箱**：脚本以当前用户权限运行，与手动在终端执行完全等价。请只
  运行你信任来源的脚本；脚本安全性由用户自担。

## 运行环境

- 解释器发现序：`PALEO_PYTHON` 环境变量 → PATH 上的 `python3` / `python`
  （与 `PythonEnvService::findBasePython()` 同序）。面板会显示实际使用的
  解释器；找不到时运行按钮禁用并给出引导。
- 子进程环境：`PYTHONUNBUFFERED=1`（保证输出流式回显）、
  `PYTHONDONTWRITEBYTECODE=1`（不在脚本目录落 `__pycache__`）。
- 工作目录：默认为脚本所在目录；脚本产出的相对路径文件落在这里。
- `argv`：面板「参数」框按空白拆分后逐个透传（不经 shell 分词）。

## JSON 行协议（可选遵守）

脚本向 **stdout** 写单行 JSON 即可汇报进度/产出/错误。**不遵守协议也能
正常运行**——所有输出按纯文本呈现在控制台，只是没有进度条与结果登记。

一行一个 JSON 对象，必须带标记 `"paleo": "1"`，且 `type` 为下列三型之一
（无标记或未知 type 的行一律按纯文本处理）：

```json
{"paleo":"1","type":"progress","percent":42,"message":"跑到一半"}
{"paleo":"1","type":"result","path":"result.geojson","kind":"geojson","message":"可选说明"}
{"paleo":"1","type":"error","message":"读不到输入","code":7}
```

| 字段 | 型 | 含义 |
|------|-----|------|
| `percent` | progress | 0..100 的整数进度（越界值由展示侧钳制） |
| `message` | 三型通用 | 人类可读说明，可省略 |
| `path` | result | 产出文件路径（相对工作目录或绝对）。**词表命中**（如 `.geojson`）的结果可在面板一键导入工程 |
| `kind` | result | 类型提示，仅展示用（`geojson`/`csv`/...） |
| `code` | error | 脚本自定义错误码，缺省 0 |

写协议行的小工具函数（示例脚本同款）：

```python
import json, sys

def paleo(**fields):
    fields["paleo"] = "1"
    # ensure_ascii 默认 True：协议行恒为 ASCII，避免遗留代码页下编码失败
    print(json.dumps(fields), file=sys.stdout, flush=True)
```

注意：**`flush=True` 不可省**——行缓冲模式下进度行会攒到进程结束才出现。
stderr 永远按纯文本分色呈现，协议解析只看 stdout。

## 示例

- `example_progress.py` —— 演示 progress/result 两型，产出一个 GeoJSON
  点要素集到工作目录（面板结果区可一键导入）。
