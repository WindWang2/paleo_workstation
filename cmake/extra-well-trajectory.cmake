# goal/well-trajectory — 井轨迹空间化：测斜域模型、最小曲率 MD↔TVD、
# 定向井贯穿消费面（剖面/平面/属性建模）。
target_sources(paleo_domain PRIVATE src/domain/deviationsurvey.cpp)

add_paleo_test(tst_deviation LIBS paleo_domain)
