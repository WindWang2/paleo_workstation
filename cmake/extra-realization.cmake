# 方向 47：多 realization / 不确定性——集合契约（catalog）+ 逐像元统计核
#（algorithms）+ 编排（workflow）+ 面板/图签（ui）。源文件与测试集中在此，
# 根 CMakeLists 只登记本文件，不改模块源列表。

target_sources(paleo_store PRIVATE src/catalog/realizationset.cpp)

# 契约：集合/成员寻址 round-trip + 缺号诚实面 + 缺键兼容（无 epoch bump）。
add_paleo_test(tst_realizationset LIBS paleo_store)
