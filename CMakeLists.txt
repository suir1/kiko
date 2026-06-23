cmake_minimum_required(VERSION 3.20)

project(kiko VERSION 0.1.6 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(DEFINED CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg\\.cmake$")
  set(KIKO_USE_VCPKG ON)
else()
  set(KIKO_USE_VCPKG OFF)
endif()

set(KIKO_DEFAULT_RELAY "106.53.170.243:9000" CACHE STRING "Default relay address when KIKO_RELAY env is unset")
add_compile_definitions("KIKO_DEFAULT_RELAY=\"${KIKO_DEFAULT_RELAY}\"")
option(KIKO_ENABLE_NETLAB_TESTS "Enable Docker-based network lab CTest targets" OFF)

set(KIKO_CORE_SOURCES
  src/adaptive.cpp
  src/ai_advisor.cpp
  src/ai_client.cpp
  src/common.cpp
  src/compression.cpp
  src/connectivity.cpp
  src/connectivity_session.cpp
  src/crypto.cpp
  src/direct_session.cpp
  src/discovery.cpp
  src/doctor.cpp
  src/file_manifest.cpp
  src/file_metadata.cpp
  src/gitignore.cpp
  src/imohash.cpp
  src/io.cpp
  src/lan_upgrade.cpp
  src/mux_receive_session.cpp
  src/mux_send_scheduler.cpp
  src/mux_transfer.cpp
  src/network_probe.cpp
  src/outbound_policy.cpp
  src/pake.cpp
  src/profile.cpp
  src/user_config.cpp
  src/tui_advanced.cpp
  src/progress.cpp
  src/protocol.cpp
  src/proxy.cpp
  src/qrcode_print.cpp
  src/relay.cpp
  src/relay_race.cpp
  src/relay_room_state.cpp
  src/relay_route_state.cpp
  src/relay_session.cpp
  src/relay_server.cpp
  src/receive_plan.cpp
  src/rendezvous_session.cpp
  src/route_planner.cpp
  src/route_session.cpp
  src/socket.cpp
  src/transfer.cpp
  src/transfer_heuristics.cpp
  src/transfer_manifest.cpp
  src/transfer_receive_paths.cpp
  src/transfer_resume.cpp
  src/transfer_retry.cpp
  src/transfer_stream.cpp
  src/udp_punch.cpp
)

add_library(kiko_core STATIC ${KIKO_CORE_SOURCES})
target_include_directories(kiko_core PUBLIC src)

if(KIKO_USE_VCPKG)
  find_package(asio CONFIG REQUIRED)
  find_package(unofficial-sodium CONFIG REQUIRED)
  find_package(zstd CONFIG REQUIRED)
  find_package(ftxui CONFIG REQUIRED)
  find_package(nlohmann_json CONFIG REQUIRED)
  find_package(CLI11 CONFIG REQUIRED)
  find_package(unofficial-nayuki-qr-code-generator CONFIG REQUIRED)
  if(TARGET zstd::libzstd)
    set(KIKO_ZSTD_TARGET zstd::libzstd)
  elseif(TARGET zstd::libzstd_shared)
    set(KIKO_ZSTD_TARGET zstd::libzstd_shared)
  else()
    message(FATAL_ERROR "zstd CMake target not found")
  endif()
  target_link_libraries(kiko_core PUBLIC asio::asio unofficial-sodium::sodium ${KIKO_ZSTD_TARGET} nlohmann_json::nlohmann_json
    unofficial::nayuki-qr-code-generator::nayuki-qr-code-generator)
  target_compile_definitions(kiko_core PUBLIC ASIO_STANDALONE KIKO_HAVE_QRCODE)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SODIUM REQUIRED libsodium)
  include(FetchContent)
  FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-32-0
    GIT_SHALLOW TRUE
  )
  FetchContent_GetProperties(asio)
  if(NOT asio_POPULATED)
    FetchContent_Populate(asio)
    add_library(asio_interface INTERFACE)
    target_include_directories(asio_interface INTERFACE ${asio_SOURCE_DIR}/asio/include)
    target_compile_definitions(asio_interface INTERFACE ASIO_STANDALONE)
    add_library(asio::asio ALIAS asio_interface)
  endif()
  find_path(ZSTD_INCLUDE_DIR zstd.h)
  find_library(ZSTD_LIBRARY NAMES zstd libzstd)
  if(NOT ZSTD_INCLUDE_DIR OR NOT ZSTD_LIBRARY)
    message(FATAL_ERROR "zstd not found. Install zstd or configure with vcpkg (see CMakePresets.json).")
  endif()
  target_include_directories(kiko_core PUBLIC ${ZSTD_INCLUDE_DIR} ${SODIUM_INCLUDE_DIRS})
  target_link_libraries(kiko_core PUBLIC ${ZSTD_LIBRARY} ${SODIUM_LIBRARIES})

  include(FetchContent)
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
  )
  FetchContent_Declare(
    CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.6.2
    GIT_SHALLOW TRUE
  )
  FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG v5.0.0
    GIT_SHALLOW TRUE
  )
  set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(nlohmann_json CLI11 ftxui)
  target_link_libraries(kiko_core PUBLIC asio::asio nlohmann_json::nlohmann_json)
  target_compile_definitions(kiko_core PUBLIC ASIO_STANDALONE)
endif()

if(APPLE)
  target_compile_definitions(kiko_core PUBLIC _DARWIN_C_SOURCE)
endif()

if(WIN32)
  target_link_libraries(kiko_core PUBLIC ws2_32 iphlpapi)
  target_compile_definitions(kiko_core PUBLIC _WIN32_WINNT=0x0601)
endif()

find_package(Threads REQUIRED)
target_link_libraries(kiko_core PUBLIC Threads::Threads)

target_compile_options(kiko_core PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>
)

set(KIKO_APP_SOURCES
  src/main.cpp
  src/tui.cpp
  src/tui_browser.cpp
  src/tui_doctor_modal.cpp
  src/tui_menu_state.cpp
  src/tui_menu_view.cpp
  src/tui_session.cpp
  src/tui_transfer_actions.cpp
  src/tui_transfer_view.cpp
  src/tui_failure_hint.cpp
  src/clipboard.cpp
)

add_executable(kiko ${KIKO_APP_SOURCES})
target_link_libraries(kiko PRIVATE kiko_core ftxui::component CLI11::CLI11)
target_compile_definitions(kiko PRIVATE "KIKO_VERSION=\"${PROJECT_VERSION}\"")

set(KIKO_RELAYD_SOURCES
  src/relayd_main.cpp
  src/adaptive.cpp
  src/common.cpp
  src/io.cpp
  src/protocol.cpp
  src/proxy.cpp
  src/relay_room_state.cpp
  src/relay_route_state.cpp
  src/relay_server.cpp
  src/socket.cpp
)

add_executable(kiko-relayd ${KIKO_RELAYD_SOURCES})
target_include_directories(kiko-relayd PRIVATE src)
target_link_libraries(kiko-relayd PRIVATE asio::asio nlohmann_json::nlohmann_json Threads::Threads)
target_compile_definitions(kiko-relayd PRIVATE ASIO_STANDALONE)
if(WIN32)
  target_link_libraries(kiko-relayd PRIVATE ws2_32 iphlpapi)
endif()

enable_testing()
add_test(NAME cli_version COMMAND $<TARGET_FILE:kiko> --version)
set_tests_properties(cli_version PROPERTIES DEPENDS kiko PASS_REGULAR_EXPRESSION "kiko ${PROJECT_VERSION}")

function(kiko_add_core_test name source)
  add_executable(kiko_${name}_test ${source})
  target_link_libraries(kiko_${name}_test PRIVATE kiko_core)
  add_test(NAME ${name} COMMAND kiko_${name}_test)
endfunction()

kiko_add_core_test(common tests/common_test.cpp)
kiko_add_core_test(doctor tests/doctor_test.cpp)
kiko_add_core_test(handshake tests/handshake_test.cpp)
kiko_add_core_test(transfer tests/transfer_test.cpp)
kiko_add_core_test(progress tests/progress_test.cpp)
kiko_add_core_test(profile tests/profile_test.cpp)
kiko_add_core_test(user_config tests/user_config_test.cpp)
kiko_add_core_test(tui_advanced tests/tui_advanced_test.cpp)
add_executable(kiko_tui_menu_state_test tests/tui_menu_state_test.cpp src/tui_menu_state.cpp src/tui_transfer_view.cpp)
target_link_libraries(kiko_tui_menu_state_test PRIVATE kiko_core ftxui::component)
add_test(NAME tui_menu_state COMMAND kiko_tui_menu_state_test)

add_executable(kiko_tui_failure_hint_test tests/tui_failure_hint_test.cpp src/tui_failure_hint.cpp)
target_link_libraries(kiko_tui_failure_hint_test PRIVATE kiko_core)
add_test(NAME tui_failure_hint COMMAND kiko_tui_failure_hint_test)
kiko_add_core_test(mux tests/mux_test.cpp)
kiko_add_core_test(adaptive tests/adaptive_test.cpp)
kiko_add_core_test(connectivity tests/connectivity_test.cpp)
kiko_add_core_test(route_session tests/route_session_test.cpp)
kiko_add_core_test(route_scenarios tests/route_scenarios_test.cpp)
kiko_add_core_test(outbound_policy tests/outbound_policy_test.cpp)
kiko_add_core_test(relay_room_state tests/relay_room_state_test.cpp)
kiko_add_core_test(relay_route_state tests/relay_route_state_test.cpp)
kiko_add_core_test(relay tests/relay_test.cpp)
kiko_add_core_test(transfer_retry tests/transfer_retry_test.cpp)
kiko_add_core_test(transfer_heuristics tests/transfer_heuristics_test.cpp)
kiko_add_core_test(ai tests/ai_test.cpp)
kiko_add_core_test(udp_punch tests/udp_punch_test.cpp)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME relay_transfer_smoke COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/relay_transfer_smoke.py $<TARGET_FILE:kiko>)
  set_tests_properties(relay_transfer_smoke PROPERTIES DEPENDS kiko TIMEOUT 120)
  add_test(NAME relayd_smoke COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/relayd_smoke.py $<TARGET_FILE:kiko-relayd>)
  set_tests_properties(relayd_smoke PROPERTIES DEPENDS kiko-relayd TIMEOUT 20)
endif()

if(NOT WIN32 AND Python3_Interpreter_FOUND)
  add_test(NAME tui_smoke COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/tui_smoke.py $<TARGET_FILE:kiko>)
  set_tests_properties(tui_smoke PROPERTIES DEPENDS kiko)
endif()

if(NOT WIN32)
  find_program(SH_EXECUTABLE sh)
  if(SH_EXECUTABLE)
    add_test(NAME install_script COMMAND ${SH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/install_script_test.sh)
  endif()
endif()

if(KIKO_ENABLE_NETLAB_TESTS)
  find_program(BASH_EXECUTABLE bash REQUIRED)
  add_test(NAME netlab_matrix COMMAND ${BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/netlab_matrix.sh)
  set_tests_properties(netlab_matrix PROPERTIES
    LABELS "netlab;docker"
    RUN_SERIAL TRUE
    TIMEOUT 300
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )
endif()
