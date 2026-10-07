# 方向 65（seismicsection 家族拆分）——新 TU 挂载点
# 根 CMakeLists 的模块表只读（seismicsectiondockwidget.cpp / seismicsectioncanvas.cpp
# 仍挂在那里）；拆分出的 13 个新 TU 经此处追加，与并行方向零冲突（同
# extra-seismic.cmake 的「源挂根表、新文件走 extra」先例）。
#
# 拆分不改任何类定义（两头文件公共区 diff 零变更），只是把成员函数的定义
# 按控制器族重新分布到多个翻译单元；链接面符号集完全等价。
target_sources(paleo_ui PRIVATE
  src/ui/seismicsection/seismicsectiondock_setup.cpp
  src/ui/seismicsection/seismicsectiondock_bookmark.cpp
  src/ui/seismicsection/seismicsectiondock_attr.cpp
  src/ui/seismicsection/seismicsectiondock_interpret.cpp
  src/ui/seismicsection/seismicsectiondock_track.cpp
  src/ui/seismicsection/seismicsectiondock_wells.cpp
  src/ui/seismicsection/seismicsectiondock_inversion.cpp
  src/ui/seismicsection/seismicsectioncanvas_params.cpp
  src/ui/seismicsection/seismicsectioncanvas_nav.cpp
  src/ui/seismicsection/seismicsectioncanvas_render.cpp
  src/ui/seismicsection/seismicsectioncanvas_paint.cpp
  src/ui/seismicsection/seismicsectioncanvas_interact.cpp
)
