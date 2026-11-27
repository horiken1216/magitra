cmake_minimum_required(VERSION 3.16)
project(MAGITRA C CXX)

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

set(MAGITRA_DEFAULT_PROFILE "generic" CACHE STRING "既定のキー割り当ての表（generic）")
# udev と XDG autostart は決まった場所しか読まないので、インストール先の prefix（/usr/local など）に関係なくここに入れる
set(MAGITRA_UDEV_DIR "/usr/lib/udev/rules.d" CACHE PATH "udev ルールの置き場所")
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
