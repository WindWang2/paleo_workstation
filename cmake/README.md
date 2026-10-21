# cmake/extra-*.cmake — 并行开发源文件挂载点

每个并行方向一个文件（如 `extra-seismic.cmake`），内容为：

```cmake
target_sources(paleo_ui PRIVATE src/ui/foo.cpp)   # 新源文件进所属模块
add_paleo_test(tst_foo)                            # 新测试
target_compile_definitions(tst_foo PRIVATE ...)    # 测试宏
```

可用 target：paleo_domain paleo_algorithms paleo_store paleo_qgis paleo_io
paleo_services paleo_linkage paleo_workflow paleo_ui paleo_app paleo_ai(条件)

## 根 CMakeLists.txt 的结构拆分（paleo-*.cmake）

根 `CMakeLists.txt` 只保留工程头、`enable_testing()` 与 `_paleo_extra_manifest`
清单，其余按原顺序 `include()` 以下文件（纯结构拆分，target/flag/测试注册不变）：

| 文件 | 内容 |
|---|---|
| `paleo-build-options.cmake` | ccache / sanitizer / LTO / unity 开关 |
| `paleo-dependencies.cmake` | QGIS/Qt/GDAL/GEOS/QScintilla 探测、4 层依赖伞、vendored 库（SARibbon/segyio/glm/sbm/ORT） |
| `paleo-modules-data.cmake` | `paleo_deps` + 数据层 + QGIS 封装层模块库 |
| `paleo-modules-ui.cmake` | 功能层 + 视图层 + 组装根 + 兼容伞 + unity 开关 |
| `paleo-test-infra.cmake` | 测试沙箱、`add_paleo_test()`、层边界护栏 |
| `paleo-tests.cmake` | 主测试注册清单 |
| `paleo-test-labels.cmake` | perf/golden/env 红名单/heavy 标签（须在所有 extra-*.cmake 之后） |

`include()` 不开新作用域，`CMAKE_SOURCE_DIR` 仍是仓库根。这些文件**不用**
`extra-` 前缀：`extra-*.cmake` 受 `_paleo_extra_manifest` 防漏对账约束。
并行方向新增源文件/测试仍然只走 `extra-<方向>.cmake`。

已知未做（范围外）：`paleo_domain` / `paleo_algorithms` 经 `paleo_deps` 间接链接
QGIS，分层契约尚未落到构建依赖上——解耦需要改源码依赖，不属于本次纯结构拆分。
