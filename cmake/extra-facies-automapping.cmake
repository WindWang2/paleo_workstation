# cmake/extra-facies-automapping.cmake — goal/facies-automapping 挂载点
#（CMakeLists.txt 模块表禁改，见 cmake/README.md）。
# 沉积相自动编图辅助链：优势相/相界提取/证据合成/QA 检测器（algorithms）
# + FaciesMappingWorkflow 编排（workflow）+ 合成/QA 面板（ui）。

target_sources(paleo_algorithms PRIVATE
  src/algorithms/faciesmapping/dominantfacies.cpp
  src/algorithms/faciesmapping/candidateboundaries.cpp
  src/algorithms/faciesmapping/evidencesynthesis.cpp
  src/algorithms/faciesmapping/faciesqa.cpp
)

target_sources(paleo_workflow PRIVATE
  src/workflow/faciesmappingworkflow.cpp
)

target_sources(paleo_ui PRIVATE
  src/ui/faciesmapping/faciesmappingpanel.cpp
)

add_paleo_test(tst_faciesmapping_algorithms LIBS paleo_algorithms)
add_paleo_test(tst_faciesmapping_workflow LIBS paleo_workflow)  # 真实栈（tst_factorworkflow 模式）：草稿相图 catalog 版本/SHA/organizer
