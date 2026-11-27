# 設定と割り当ての解析は /dev に触れないので、単体テストのためライブラリに分ける
add_library(magitrad_settings STATIC settings.c binding.c pathprefix.c)
target_include_directories(magitrad_settings PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(magitrad_settings PUBLIC MAGITRA_DEFAULT_PROFILE="${MAGITRA_DEFAULT_PROFILE}")
target_link_libraries(magitrad_settings PUBLIC magitra_common)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

add_executable(magitrad main.c device.c uinput.c actions.c)
target_link_libraries(magitrad PRIVATE magitrad_settings PkgConfig::SYSTEMD)
target_compile_definitions(magitrad PRIVATE MAGITRA_VERSION="${PROJECT_VERSION}")

add_executable(test_settings tests/test_settings.c)
target_link_libraries(test_settings PRIVATE magitrad_settings)
add_test(NAME test_settings COMMAND test_settings)

install(TARGETS magitrad RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
# ExecStart にインストール先の bin を入れる
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/../data/magitrad.service" "${CMAKE_CURRENT_BINARY_DIR}/magitrad.service" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/magitrad.service" DESTINATION "${MAGITRA_SYSTEMD_DIR}")
