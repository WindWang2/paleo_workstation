# V0：本地依赖来源报告

`tools/audit_dependency_origins.py` 收集证据，不重写配置、改动加载器环境或放行
系统库。每次采集写一个新的 JSON，含仓库提交、锁的 SHA-256、OS、编译器和检测结果。
值为 `outside` 或 `missing` 的条目留到对应迁移批处理；报告成功只说明采集完成。

在干净的构建目录先创建 `.cmake/api/v1/query/codemodel-v2` 空文件，再照该路线
配置、构建。下面的例子适用于 Linux 主程序；根据本机实际路线替换受控前缀。

```sh
mkdir -p build-audit/.cmake/api/v1/query
touch build-audit/.cmake/api/v1/query/codemodel-v2
# 用这条路线的依赖配置运行 cmake 和构建。运行审计前使用相同的加载环境。
python3 -B tools/audit_dependency_origins.py --build build-audit --route linux-main \
  --prefix project="$PWD/vendor/prefix/usr" --prefix ort="$PWD/vendor/onnxruntime" \
  --prefix build="$PWD/build-audit" --lock vendor/deb-closure.lock \
  --elf build-audit/paleo --output local/linux-main-origins.json
```

Windows 改用 `--pe build-audit/paleo.exe --dumpbin <VS 工具完整路径>` 并按实际查找
顺序重复传入 `--dll-dir`，包括 Qt、QGIS、ORT 及系统目录（路径存在并不代表
获准）。静态 PE 报告按显式路径找文件。没有找到物理 DLL 的 API-set 名称单列为
`api-set-unresolved`，实际映射以加载器观测为准。这些名称不自动进入宿主 ABI 清单。
Linux 的 `--elf` 只用于自己构建、已知可信的二进制。重复 `--loaded consumer=file`
可合并观测：Windows 读取本地同用户进程的 `Modules.FileName`；Linux 从
`/proc/<pid>/maps` 取得共享库的绝对文件路径；Qt 插件日志中确认已加载的路径另作为
有名字的 consumer 导入。每行一个绝对路径，按同一场景复跑，不从选择顺序推导加载。

分别对主构建、SDK 独立构建、native-perf、superbuild 前缀、发行树取样。对安装树
传入生成该产物的 build 和安装树中的二进制，不把 build 根加入安装路径白名单。
`--abi NAME=FILE` 只承认该名字实际解析到这个文件；白名单不要按目录或通配设置。
File API 未启用时报告会标明缺这一层，配置缓存结果不会冒充编译/链接结果。
`link` 行来自 CMake 生成的绝对库参数，不证明隐式 `-l` 或库内部依赖解析；
对这类情况以实际 ELF/PE 结果或链接映射补证。若某条路线尚未构建、尚未采样插件
或用的是复用产物，分别说明缺项，不记作通过。

采集表的最小验证：`python -B tools/test_audit_dependency_origins.py`。
