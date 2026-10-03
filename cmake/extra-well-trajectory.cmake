# goal/well-trajectory — 井轨迹空间化：测斜域模型、最小曲率 MD↔TVD、
# 定向井贯穿消费面（剖面/平面/属性建模）。
target_sources(paleo_domain PRIVATE src/domain/deviationsurvey.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/welltrajectorylayer.cpp)

add_paleo_test(tst_deviation LIBS paleo_domain)
add_paleo_test(tst_deviation_import LIBS paleo_io)
add_paleo_test(tst_welllogset_tvd LIBS paleo_services)
add_paleo_test(tst_sectiontrajectory LIBS paleo_workflow)
add_paleo_test(tst_deviation_realarea LIBS paleo_workflow) # 真工区回退面（PALEO_REAL_PROJECT_AREA 门控，未设跳过）
