# cmake/extra-*.cmake — 并行开发源文件挂载点

每个并行方向一个文件（如 `extra-seismic.cmake`），内容为：

```cmake
target_sources(paleo_ui PRIVATE src/ui/foo.cpp)   # 新源文件进所属模块
add_paleo_test(tst_foo)                            # 新测试
target_compile_definitions(tst_foo PRIVATE ...)    # 测试宏
```

可用 target：paleo_domain paleo_algorithms paleo_store paleo_qgis paleo_io
paleo_services paleo_linkage paleo_workflow paleo_ui paleo_app paleo_ai(条件)
