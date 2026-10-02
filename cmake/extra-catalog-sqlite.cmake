# goal/catalog-sqlite — catalog 持久层换 SQLite（内存模型与查询 API 不变）。
target_sources(paleo_store PRIVATE
  src/catalog/catalogstore.cpp)

add_paleo_test(tst_catalogstore LIBS paleo_store)
add_paleo_test(tst_datacatalog_sqlite LIBS paleo_store)
