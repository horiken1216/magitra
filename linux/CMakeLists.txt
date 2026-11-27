cmake_minimum_required(VERSION 3.16)
# 版。リリース（.github/workflows/release.yml）はタグの版（v を除く）を -DMAGITRA_VERSION で渡す
set(MAGITRA_VERSION "0.4.0" CACHE STRING "版（.deb と magitrad --version）")
project(MAGITRA VERSION ${MAGITRA_VERSION} LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# C だけ厳しく見る。_GNU_SOURCE は全 C ソースに同じ定義を渡す（ソース側は #ifndef で守ってある）
add_compile_options(
    "$<$<COMPILE_LANGUAGE:C>:-Wall>"
    "$<$<COMPILE_LANGUAGE:C>:-Wextra>"
    "$<$<COMPILE_LANGUAGE:C>:-Werror>"
    "$<$<COMPILE_LANGUAGE:C>:-D_GNU_SOURCE>")

set(MAGITRA_DEFAULT_PROFILE "generic" CACHE STRING "既定のキー割り当ての表（generic か lxqt）")
set_property(CACHE MAGITRA_DEFAULT_PROFILE PROPERTY STRINGS generic lxqt)
if(NOT MAGITRA_DEFAULT_PROFILE MATCHES "^(generic|lxqt)$")
    message(FATAL_ERROR "MAGITRA_DEFAULT_PROFILE は generic か lxqt: ${MAGITRA_DEFAULT_PROFILE}")
endif()
# systemd と XDG autostart は決まった場所しか読まないので、インストール先の prefix（/usr/local など）に関係なくここに入れる
set(MAGITRA_SYSTEMD_DIR "/usr/lib/systemd/system" CACHE PATH "systemd のサービスファイルの置き場所")
set(MAGITRA_AUTOSTART_DIR "/etc/xdg/autostart" CACHE PATH "XDG 自動起動の .desktop の置き場所")
option(MAGITRA_BUILD_GUI "magitra-gui（Qt6）をビルドする" ON)

enable_testing()

set(MAGITRA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Windows 版と共有する、OS に依存しない部分（コピーせず直接コンパイルする）
add_library(magitra_common STATIC
    "${MAGITRA_ROOT}/core/src/mtp_report.c"
    "${MAGITRA_ROOT}/core/src/mtp_haptic.c"
    "${MAGITRA_ROOT}/agent/mtp_gesture.c"
    "${MAGITRA_ROOT}/agent/mtp_momentum.c")
target_include_directories(magitra_common PUBLIC "${MAGITRA_ROOT}/core/include" "${MAGITRA_ROOT}/agent")
target_link_libraries(magitra_common PUBLIC m)

foreach(t IN ITEMS
        "core/tests/test_report.c"
        "agent/tests/test_gesture.c"
        "agent/tests/test_inertia.c")
    get_filename_component(name "${t}" NAME_WE)
    add_executable(${name} "${MAGITRA_ROOT}/${t}")
    target_compile_options(${name} PRIVATE "$<$<COMPILE_LANGUAGE:C>:-Wno-missing-field-initializers>")
    target_link_libraries(${name} PRIVATE magitra_common m)
    add_test(NAME ${name} COMMAND ${name})
endforeach()

if(MAGITRA_BUILD_GUI)
    add_subdirectory(gui)
endif()
add_subdirectory(daemon)

# Debian パッケージ（cpack -G DEB）。LXQt 版は magitra-lxqt という名前にする
set(CPACK_PACKAGE_NAME "magitra")
if(MAGITRA_DEFAULT_PROFILE STREQUAL "lxqt")
    set(CPACK_PACKAGE_NAME "magitra-lxqt")
endif()
set(CPACK_PACKAGE_CONTACT "MAGITRA <https://github.com/horiken1216/magitra>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Apple Magic Trackpad の常駐アプリ（ジェスチャー・Force Touch・慣性スクロール）")
# パッケージ内のパスは configure の prefix と同じにする（サービスファイルの ExecStart は configure 時に決まるため）。
# .deb を作るときは -DCMAKE_INSTALL_PREFIX=/usr で configure する
set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
    message(STATUS "CMAKE_INSTALL_PREFIX が /usr ではない（${CMAKE_INSTALL_PREFIX}）。.deb を作るなら -DCMAKE_INSTALL_PREFIX=/usr にする")
endif()
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# init-system-helpers: postinst などの deb-systemd-helper・deb-systemd-invoke。libglib2.0-bin: ランチャーがアプリを起動する gio
set(CPACK_DEBIAN_PACKAGE_DEPENDS "init-system-helpers (>= 1.52), libglib2.0-bin")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_SOURCE_DIR}/data/postinst;${CMAKE_CURRENT_SOURCE_DIR}/data/prerm;${CMAKE_CURRENT_SOURCE_DIR}/data/postrm")
include(CPack)
