# 地震属性优选与融合客户端

`paleo::SeismicAttributeFusionClient` 是数据层的同步 C++ 调用入口，调用方须放在
任务 worker 中。它对接独立部署的本机服务 API 1.0，不启动 HTTP 服务、不引入
Python 运行基础，也不替换已有的原生地震预测。此批交付客户端和协议测试，
尚未接入界面、目录发布或模型版本登记。

## 配置与调用

默认地址 `http://127.0.0.1:3210`；只接受回环 HTTP 根地址，禁用代理和重定向。
`GET /health` 须返回 `status=ok`、`service=seismic-attribute-fusion` 和
`api_version=1.0`。随后 `POST /api/v1/jobs` 提交 `wait=false`，根据返回的
`job_id` 轮询 `GET /api/v1/jobs/{id}`。提交后任何失败都不会再回退 CLI，
避免一次请求执行两份计算。失败保留服务的错误说明。

CLI 是显式配置的可选路径：HTTP 不可用且 `scriptPath` 或环境变量
`PALEO_FUSION_SCRIPT` 非空时，才调用外部 `service_server.py --run ... --result ...`。
`serverUrl` 置空可直接使用 CLI。`pythonExe` 优先，其次是
`PALEO_FUSION_PYTHON`、所配脚本同目录的 `.venv`、PATH 中的 python3/python。
显式指定的无效路径会返回错误，不搜索开发者个人目录。

```cpp
paleo::SeismicFusionRequest request;
request.seismicPath = seismicPath;
request.topHorizonPath = topPath;
request.bottomHorizonPath = bottomPath;
request.outputDir = outputDirectory;
request.targetCrs = targetCrs;
// 在任务 worker 内；回调也在该线程，视图更新应经既有任务信号传回。
const auto result = paleo::SeismicAttributeFusionClient::execute(request, progress, cancelled);
```

输入至少含 SEG-Y 与单层位、顶底层位或预对齐层段窗中的一种；井文件选填。
文件路径在跨进程前转为绝对路径。聚类数 2–32；正网格间距；层段编号 0–4
（0 为全层段），显式比例须满足 `0 <= top < bottom <= 1`。`bbox` 采用
`QRectF(xmin, ymin, width, height)`，传输为 `[xmin, ymin, xmax, ymax]`。
服务的其他高级训练参数尚未暴露在该类型中。

## 生命周期与结果

默认总预算 10 分钟，覆盖健康检查、提交、轮询/子进程和二进制结果读取。
健康检查、提交和单次轮询另有 1.2/10/5 秒上限。取消在这些阶段均可检查；
已取得任务 ID 时等待取消请求完成，清理另有最多 1 秒宽限。进程取消/超时后
kill 并等待退出；请求和结果控制文件自动删除。CLI 未指定输出目录时，成功
结果目录保留供调用方消费，失败时自动清理；指定目录的成果由调用方管理。

HTTP 取消是服务的合作式请求，不等于确认服务已经停算。若提交响应丢失而
尚未取得任务 ID，API 1.0 没有按请求键恢复或取消的能力，客户端也不会重试提交。

结果只在完整校验后设置 `ok=true`，包括：

- 直接结果对象及 HTTP/CLI 的 `state/result` 包装，失败状态不能被内部成功字段覆盖。
- 正整数行列、像元间距与外沿范围一致、六参数北向上变换、东西/北向 XY 米制轴序。
- CRS 从真实协议的 `grid.crs_auth` / `grid.crs_wkt` 读取；未知时保留为空，不猜坐标系。
- 必需 GeoTIFF/二进制及声明的其他成果文件可读；相对成果路径按输出目录解析。
- 二进制长度精确相符，按小端 int32 解码；编码为 `1..K` 或 `-9999`，有效像元数一致。
- 默认最多 16,777,216 像元（int32 约 64 MiB），调用方可设更小预算，硬上限
  67,108,864 像元；JSON 上限 8 MiB，二进制分块读取并检查取消。

`extent` 的 y 是南侧外沿，`gdalGeoTransform[3]` 是北侧外沿；`cellsNorthUp`
第 0 行为最北行，NPZ 的南向上排列不会混入该数组。聚类编号按服务的平均属性值
排序，不是地质相命名；地质语义映射、GIS 图层接管和版本发布仍应走既有功能层。
此客户端验证返回元数据与二进制，不在此处重复实现 GeoTIFF 的 GIS 解码。

## 验证入口

主工程注册 `add_paleo_test(tst_seismicattributefusion LIBS paleo_services)`。
无 GIS 依赖时可单独编译同一份生产源和同一 QtTest：

```sh
cmake -S tools/native-fusion -B build-native-fusion -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-native-fusion -j8
ctest --test-dir build-native-fusion -j1 --output-on-failure
```

需要 Qt6 Core/Network/Test 与测试用 Python；Windows 同样应按 BUILDING.md 验证
MSVC/Ninja 的本地化依赖前缀。协议测试使用临时回环 HTTP 服务和真实 Python 子进程，
不依赖私有资料、训练库或长期运行的本机服务。夹具的 TIFF 只用于文件存在性校验；
该测试不冒充真实融合模型、GeoTIFF 解码或应用界面端到端验收。
