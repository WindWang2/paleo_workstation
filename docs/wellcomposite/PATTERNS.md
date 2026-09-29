# 岩性花纹与相纹理图例（PATTERNS）

> wave/wellcomposite-deep D4.2/D4.3。程序化纹理（QPainter 16/20px tile），
> 无位图资源依赖；本文件即图例文档。

## 1. 岩性花纹（≥30 种，PatternCatalog）

按关键词最长匹配（`PatternCatalog::lookupLithology`）；底色/纹色遵循地质
惯例（数据符号色，不占 UI token）。截断测试词如「泥质砂岩」→
argillaceous_sandstone（长词优先）。

### 碎屑岩
| 花纹键 | 匹配词 | 底色 | 纹理 |
|---|---|---|---|
| sandstone | 砂岩/砂 | #FFF9C4 | 细均匀点 |
| fine_sandstone | 细砂岩 | #FFFDE7 | 密细点 |
| coarse_sandstone | 粗砂岩/含砾砂岩 | #FFF3C4 | 粗点 |
| siltstone | 粉砂岩/粉砂 | #FFFDE7 | 点+短虚线 |
| mudstone | 泥岩/泥 | #ECEFF1 | 横向短细线 |
| shale | 页岩/页理 | #E0E7EB | 密集水平页理线 |
| sandy_mudstone | 砂质泥岩/含砂泥岩 | #F1F0E4 | 横线+砂点 |
| argillaceous_sandstone | 泥质砂岩 | #FAF0D8 | 横线+点 |
| conglomerate | 砾岩/砾 | #FFF8E1 | 小圆圈 |
| breccia | 角砾岩/角砾 | #F6EBD9 | 棱角多边形 |
| glauconitic_sodstone* | 海绿石砂岩/绿砂 | #E8F5E9 | 实心绿点 |

\* 键名 `glauconitic_sandstone`。

### 碳酸盐岩
| 花纹键 | 匹配词 | 纹理 |
|---|---|---|
| limestone | 灰岩/石灰岩 | 错缝砖形纹 |
| oolitic_limestone | 鲕粒灰岩/鲕状 | 同心小圆（鲕粒） |
| bioclastic_limestone | 生物灰岩/生物碎屑 | 弧形壳片 |
| reef_limestone | 礁灰岩/礁 | 格架网状 |
| chalk | 白垩 | 细点阵 |
| marl | 泥灰岩/泥灰 | 横线+散点 |
| dolomite | 白云岩/云岩 | 斜向错缝斜砖纹 |
| dolomitic_limestone | 白云质灰岩 | 砖纹+斜线复合 |
| calcirudite | 灰砾岩/砾屑灰岩 | 大弧片 |
| shoal_grainstone | 颗粒滩/滩灰岩/滩 | 颗粒圆点（滩相） |

### 蒸发岩
| gypsum | 石膏/膏岩 | 斜向长条晶 |
| anhydrite | 硬石膏 | 斜向长条晶（深） |
| salt_rock | 盐岩/石盐 | 立方晶格 |

### 有机岩
| coal | 煤 | 密条纹（既有） |
| oil_shale | 油页岩 | 层理线+油滴 |
| carbonaceous_mudstone | 碳质泥岩/炭质 | 层理线+碳线 |

### 火成岩
| tuff | 凝灰岩/凝灰 | 细点+小棱块 |
| tuffaceous_sandstone | 凝灰质砂岩 | 点+横线 |
| basalt | 玄武岩 | 柱状节理 |
| andesite | 安山岩 | 流面弧线 |
| rhyolite | 流纹岩 | 流面弧线 |
| granite | 花岗岩/基岩 | 三向节理交错 |
| pyroclastic | 火山碎屑岩 | 点+棱块 |

### 其他
| phosphorite | 磷块岩/磷矿 | 圆点+线 |
| chert | 硅质岩/燧石 | 贝壳断口弧 |
| ironstone | 铁质岩/赤铁矿层 | 实心圆点 |
| diatomite | 硅藻土 | 稀点阵 |

## 2. 沉积相纹理（FaciesPatternFactory）

既有 11 种程序纹理（水下分流河道/河口坝/席状砂/分流间湾/三角洲前缘/
三角洲平原/前三角洲/浅海陆棚/浊积/滞留/潮坪）+ FaciesCatalog SVG 资源
优先。D4.3 外置映射后新增：颗粒滩 shoal_grainstone、礁 reef_limestone、
泻湖 marl、沼泽 carbonaceous_mudstone。

## 3. 相名→花纹 JSON 映射（D4.3）

- 内置：代码 `builtinFaciesMap()` 与
  `src/ui/wellcomposite/resources/faciespatterns.json` 同源。
- 用户覆盖：`QStandardPaths::AppConfigLocation/wellcomposite/faciespatterns.json`
  （本机便携安装为 `<数据目录>/wellcomposite/faciespatterns.json`），
  用户条目优先；`PatternCatalog::reloadFaciesMap()` 重载。
- 未映射相名回落 Factory 旧关键词匹配；再不中用默认纹理（不臆造空纹）。

## 4. 年代地层色标（D4.1，ChronostratColors）

国际年代地层表（ICS）色系规整化：14 系 + 40 统 + 珠江口盆地 13 区域组
映射（粤海组/万山组/韩江组/珠江组/珠海组/恩平组/文昌组/神狐组…）。
`lookupByFormation(组名) → (系, 统)`；未识别返回空（D3.6 用户显式指派）。
