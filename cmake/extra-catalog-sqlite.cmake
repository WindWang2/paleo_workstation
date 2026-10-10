# goal/catalog-sqlite — catalog 持久层换 SQLite（内存模型与查询 API 不变）。
# 方向 99 拆分：主 TU（open 编排/迁移/恢复）+ _schema（连接/DDL/事务）+
# _crud（upsert/delete/meta 写）+ _json（JSON 编解码/四表装载）；共享助手
# 在 catalogstore_internal.h（paleo::catalog_detail）。
target_sources(paleo_store PRIVATE
  src/catalog/catalogstore.cpp
  src/catalog/catalogstore_schema.cpp
  src/catalog/catalogstore_crud.cpp
  src/catalog/catalogstore_json.cpp)

add_paleo_test(tst_catalogstore LIBS paleo_store)
add_paleo_test(tst_datacatalog_sqlite LIBS paleo_store)
