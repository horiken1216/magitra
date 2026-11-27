# magitra-gui（Qt6 Widgets + Network）。設定の読み書きは guisettings.cpp（QtCore だけ）に分けてあり、単体テストできる
find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Network)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT DEFINED MAGITRA_DEFAULT_PROFILE)
  set(MAGITRA_DEFAULT_PROFILE "generic")
endif()

add_library(magitra_guisettings STATIC guisettings.cpp)
target_link_libraries(magitra_guisettings PUBLIC Qt6::Core)
target_compile_definitions(magitra_guisettings PUBLIC MAGITRA_DEFAULT_PROFILE="${MAGITRA_DEFAULT_PROFILE}")
set_target_properties(magitra_guisettings PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(magitra-gui
  main.cpp
  agentclient.cpp
  launcherwindow.cpp
  settingswindow.cpp
  resources.qrc)
target_link_libraries(magitra-gui PRIVATE magitra_guisettings Qt6::Widgets Qt6::Network)
target_compile_features(magitra-gui PRIVATE cxx_std_17)
target_compile_options(magitra-gui PRIVATE -Wall -Wextra)

add_executable(test_guisettings tests/test_guisettings.cpp)
target_link_libraries(test_guisettings PRIVATE magitra_guisettings Qt6::Core)
target_compile_features(test_guisettings PRIVATE cxx_std_17)
add_test(NAME guisettings COMMAND test_guisettings)

include(GNUInstallDirs)
install(TARGETS magitra-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ../data/magitra.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
install(FILES ../data/magitra-gui.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
if(NOT DEFINED MAGITRA_AUTOSTART_DIR)
  set(MAGITRA_AUTOSTART_DIR "/etc/xdg/autostart")
endif()
install(FILES ../data/magitra-tray.desktop DESTINATION ${MAGITRA_AUTOSTART_DIR})
