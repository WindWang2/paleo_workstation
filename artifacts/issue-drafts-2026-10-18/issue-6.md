# [med] 原生属性表改编码同步绕过 #526 修复：旧七列草稿被写入错位颜色/纹理

## 问题

本次新增的「原生属性表改编码同步三级相属性」直接拿 version.extra 里的原始 schema 喂 FaciesCatalog::attributes()，绕过了 #526 的 recoverLegacyAppearanceColumns 修复：对仍存旧七列错位元组的草稿，会把颜色值写进图层的 microfacies 属性、把 SVG 路径留在 color、texture 置空，而渲染器/图例（走 place() 已修复）显示的是正确名称与纹理——同一次编辑产出自相矛盾的属性与图面。

## 证据

- 当前 HEAD 逐行读过 `src/workflow/mappingevidencereview.cpp:753-761`：`const auto attrs = FaciesCatalog::attributes(versionForLayer(id).extra.value("facies").toList(), f.attribute("facies_code"));` 随后 :757-760 把 attrs 逐个 changeAttributeValue 写进图层。
- src/domain/faciescatalog.cpp:126-133 的 attributes() 原样返回 microfacies / facies_label / texture，不做修复；recoverLegacyAppearanceColumns 只在 src/domain/faciestaxonomy.cpp:12-27，被 :60 place() 与 :185 normalize() 调用。
- 渲染侧 src/qgis/facieshierarchyrenderer.cpp:75 + src/domain/facieshierarchy.cpp:60 走 place() 已修复 → 读写分叉。
- 触发窗口：只改了编码但未保存版本（copyForEditing / saveEditingVersion 会用 normalize 后的 schema 覆盖一次属性），且草稿版本 extra 建于 #526 之前才触发。

## 影响

用户在原生属性表改一次编码，图层属性与图面/图例即互相矛盾；错位值（SVG 路径进 color）还会被后续导出与检索消费。

## 建议

在调 FaciesCatalog::attributes() 前先对 version schema 过一次 FaciesTaxonomy::normalize()（或在草稿装载/ensure 时一次性把 extra.facies 正规化后再用），使写路径与 place() 读路径同一口径。

## 验证

构造一个 #526 之前的旧七列 extra.facies 草稿版本，走原生属性表改编码，断言图层 microfacies / facies_label / texture / color 与 FaciesTaxonomy::place() 渲染取值一致。

## 关联

- 修复来源：#526（还原旧分类错位的名称颜色和纹理）；本次回归面：mappingevidencereview 写路径。
