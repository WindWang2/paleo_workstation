# 缓存体系设计（wave/io-perf-cache P4）

## 1. 分层

```
调用方（previewdoc / dataimportservice / 自检）
   │
   ▼
业务缓存            LasCache（D1）          SegyIndexStore（D2）     RasterPyramidService（D3）   ShaCache（D7.7）
   │  指纹 = canonical path + mtime + size   │  身份 = path+size+mtime+inode + 前缀指纹   │  源 mtime/size      │  mtime+size
   ▼
内存治理            LruCache<K,V>（io/lrucache.h）──注册──▶ CacheBudgetManager（io/cachebudget，D6）
   │                                            ▲ EvictableCache 接口（id/bytes/evictLRU/stats/lastAccess）
   ▼
磁盘底座            cachecore（io/cachecore）：36B 版本化头 + CRC32×2 + zstd + 原子发布（mkdir -p + tmp + rename + 重试）
```

## 2. 统一失效语义（D1.2/D2.2）

一切键 = 内容身份指纹，不存 TTL：

| 变化 | 检测点 | 动作 |
|---|---|---|
| mtime / size 变 | 每次访问 stat 比对 | 旧条目作废，重建 |
| 文件被改写但 mtime 伪造 | SEG-Y 追加场景用前缀指纹（首 64KB SHA-256，D2.5） | 前缀不同 → 全重建；相同且只增长 → 断点续扫 |
| inode 变（换文件同名） | SEG-Y 身份块（D2.2） | 重建 |
| 缓存文件损坏/过版 | 读侧 magic/版本/CRC/尺寸五道闸（D2.3） | **删除自愈** + 重建，selfHeals 计数 |
| 显式写路径 | 调用方 `invalidate(path)`（D1.10） | 内存+磁盘同清 |

## 3. LRU + 预算治理（D6）

- `LruCache`：std::list（MRU 头）+ QHash 索引；`get` 触摸、`peek` 只读；
  `setPin` 钉住（正在使用的瓦片/文档不被逐出，pinnedSkips 计数）。
- 注册即治理：构造 `registerCache`、析构 `unregisterCache`；每次 `insert`
  触发 `CacheBudgetManager::enforce()`——超限按「最远未用缓存先收缩」逐到
  预算 90%；跨 75%/90% 两档向 `PressureHandler` 广播（不刷屏：同档不重发）。
- 统计面：每缓存 hits/misses/evictions/diskHits/diskWrites/selfHeals
  （D6.5，selfcheck 与测试直接读）。
- 大对象审计（D6.6）：>10MB 单次读取 `noteLargeAllocation` 登记（环形 256
  条）；>500MB LAS 拒绝整读走流式（D1.8）。

## 4. 并发合并（D4.7）

`InflightCoalescer<Key, Result>`（io/inflight.h）：同键并发提交，首个成为
执行者同步跑 job，其余拿 shared_future 等待同一结果（LasDoc 内 QVector 是
COW，浅拷贝零拷）。异常入 future 重抛，绝不留挂死 promise。GUI 线程语义：
同步降级路径无并发提交者，天然不等待。

## 5. 各缓存要点

### LasCache（D1.1/D1.2/D1.10）
- 内存 LRU（默认 64MB）→ 磁盘 `<proj>/artifacts/index/las/<sha256[0:24]>.plc`
  （曲线元数据 + f64 列，zstd 可用则压缩）→ 冷解析回填两级。
- 载荷校验五道闸 + 指纹串内嵌（防哈希碰撞错配）。
- 性能口径：15581 点冷 <50ms、磁盘命中 <5ms、内存命中 <1ms（基线实测
  16/0.05/0.03ms）。

### SegyIndexStore（D2）
- 完整索引：身份命中免扫（真实 1013MB 体 449ms→21ms）。
- checkpoint：取消/崩溃落部分索引（complete=false + scannedOffset），重开
  断点续扫；审计闸（scannedOffset 越界/道覆盖不符 → 弃）。
- 增量：文件追加（前缀指纹吻合 + inode 同）续扫；改写 → 全重建。
- zstd 压缩 -87%（真实体索引 4.2MB→0.53MB 量级，合成测得 ratio 0.126）。

### RasterPyramidService（D3）
- 瓦片 256×256 Float32，层级 z 缩 2^z；懒建（首次 tile() 生成）/预建可选。
- 全 nodata 瓦片不落地（state=2 无载荷）；追加式层级包（崩溃残片只留未建态）。
- 同源构建互斥、跨源并行；GDAL dataset 每次构建独立打开。

### ShaCache（D7.7）
- 内存指纹表 + 磁盘 `sha.json`（path→mtime|size|sha，LRU 65k 封顶）；
  导入去重/外链复验的重复全量重哈希（966MB×N）变成 stat 级。

## 6. 已知边界

- 预算治理挡 include 层，挡不住「不带 include 直接 new」的内存（与分层护栏
  同一遗留口径，见 TODOS）。
- WriteGuard（D7.5）是同工程目录协作锁（pid 存活探测），非网络级锁。
- vendor SgyIndexCache 与本体系并存：vendor 路径只做目录预建根治（D2.1），
  不改 vendor 代码。
