cmake_minimum_required(VERSION 3.20)

project(kiko VERSION 0.2.1 LANGUAGES CXX)

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

set(KIKO_CORE_LAYER_SOURCES
  src/core/adaptive.cpp
  src/core/cancellation.cpp
  src/core/common.cpp
  src/core/compression.cpp
  src/core/crypto.cpp
  src/core/imohash.cpp
  src/core/io.cpp
  src/core/network_interfaces.cpp
  src/core/pake.cpp
  src/core/progress.cpp
  src/core/progress_state.cpp
  src/core/protocol.cpp
  src/core/proxy.cpp
  src/core/qrcode_print.cpp
  src/core/socket.cpp
)

set(KIKO_PLATFORM_LAYER_SOURCES
  src/platform/path_browser.cpp
  src/platform/user_config.cpp
)

set(KIKO_CONNECT_LAYER_SOURCES
  src/connect/connectivity.cpp
  src/connect/connectivity_session.cpp
  src/connect/direct_session.cpp
  src/connect/discovery.cpp
  src/connect/encrypted_session.cpp
  src/connect/lan_upgrade.cpp
  src/connect/peer_route_session.cpp
  src/connect/peer_session.cpp
  src/connect/profile.cpp
  src/connect/rendezvous_session.cpp
  src/connect/route_planner.cpp
  src/connect/route_session.cpp
  src/connect/udp_punch.cpp
)

set(KIKO_RELAY_LAYER_SOURCES
  src/relay/relay.cpp
  src/relay/relay_protocol.cpp
  src/relay/relay_race.cpp
  src/relay/relay_room_state.cpp
  src/relay/relay_route_state.cpp
  src/relay/relay_server.cpp
)

set(KIKO_TRANSFER_LAYER_SOURCES
  src/transfer/file_manifest.cpp
  src/transfer/file_metadata.cpp
  src/transfer/gitignore.cpp
  src/transfer/mux_receive_session.cpp
  src/transfer/mux_send_scheduler.cpp
  src/transfer/mux_transfer.cpp
  src/transfer/receive_plan.cpp
  src/transfer/transfer.cpp
  src/transfer/transfer_file_session.cpp
  src/transfer/transfer_heuristics.cpp
  src/transfer/transfer_manifest.cpp
  src/transfer/transfer_receive_paths.cpp
  src/transfer/transfer_resume.cpp
  src/transfer/transfer_retry.cpp
  src/transfer/transfer_stream.cpp
)

set(KIKO_NOTE_LAYER_SOURCES
  src/note/note_protocol.cpp
  src/note/note_session.cpp
  src/note/note_workspace.cpp
  src/note/notepad.cpp
)

set(KIKO_DIAGNOSTICS_LAYER_SOURCES
  src/diagnostics/ai_advisor.cpp
  src/diagnostics/ai_client.cpp
  src/diagnostics/doctor.cpp
  src/diagnostics/network_probe.cpp
  src/diagnostics/outbound_policy.cpp
)

set(KIKO_TUI_LAYER_SOURCES
  src/tui/tui_advanced.cpp
)

set(KIKO_CORE_SOURCES
  ${KIKO_CORE_LAYER_SOURCES}
  ${KIKO_PLATFORM_LAYER_SOURCES}
  ${KIKO_CONNECT_LAYER_SOURCES}
  ${KIKO_RELAY_LAYER_SOURCES}
  ${KIKO_TRANSFER_LAYER_SOURCES}
  ${KIKO_NOTE_LAYER_SOURCES}
  ${KIKO_DIAGNOSTICS_LAYER_SOURCES}
  ${KIKO_TUI_LAYER_SOURCES}
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
  src/app/main.cpp
  src/tui/tui.cpp
  src/tui/tui_browser.cpp
  src/tui/tui_doctor_modal.cpp
  src/tui/tui_menu_state.cpp
  src/tui/tui_menu_view.cpp
  src/tui/tui_session.cpp
  src/tui/tui_transfer_actions.cpp
  src/tui/tui_transfer_view.cpp
  src/tui/tui_failure_hint.cpp
  src/tui/tui_note.cpp
  src/web/web.cpp
  src/web/web_assets.cpp
  src/web/web_job.cpp
)

add_executable(kiko ${KIKO_APP_SOURCES})
target_link_libraries(kiko PRIVATE kiko_core ftxui::component CLI11::CLI11)
target_compile_definitions(kiko PRIVATE "KIKO_VERSION=\"${PROJECT_VERSION}\"")

set(KIKO_RELAYD_SOURCES
  src/app/relayd_main.cpp
  src/core/adaptive.cpp
  src/core/common.cpp
  src/core/io.cpp
  src/core/protocol.cpp
  src/core/proxy.cpp
  src/relay/relay_protocol.cpp
  src/relay/relay_room_state.cpp
  src/relay/relay_route_state.cpp
  src/relay/relay_server.cpp
  src/core/socket.cpp
)

add_executable(kiko-relayd ${KIKO_RELAYD_SOURCES})
target_include_directories(kiko-relayd PRIVATE src)
target_link_libraries(kiko-relayd PRIVATE asio::asio nlohmann_json::nlohmann_json Threads::Threads)
target_compile_definitions(kiko-relayd PRIVATE ASIO_STANDALONE "KIKO_VERSION=\"${PROJECT_VERSION}\"")
if(WIN32)
  target_link_libraries(kiko-relayd PRIVATE ws2_32 iphlpapi)
endif()

enable_testing()
add_test(NAME cli_version COMMAND $<TARGET_FILE:kiko> --version)
set_tests_properties(cli_version PROPERTIES DEPENDS kiko PASS_REGULAR_EXPRESSION "kiko ${PROJECT_VERSION}")
add_test(NAME relayd_version COMMAND $<TARGET_FILE:kiko-relayd> --version)
set_tests_properties(relayd_version PROPERTIES DEPENDS kiko-relayd PASS_REGULAR_EXPRESSION "kiko-relayd ${PROJECT_VERSION}")

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
kiko_add_core_test(progress_state tests/progress_state_test.cpp)
kiko_add_core_test(profile tests/profile_test.cpp)
kiko_add_core_test(user_config tests/user_config_test.cpp)
kiko_add_core_test(path_browser tests/path_browser_test.cpp)
kiko_add_core_test(cancellation tests/cancellation_test.cpp)
kiko_add_core_test(tui_advanced tests/tui_advanced_test.cpp)
add_executable(kiko_tui_menu_state_test tests/tui_menu_state_test.cpp src/tui/tui_menu_state.cpp src/tui/tui_transfer_view.cpp)
target_link_libraries(kiko_tui_menu_state_test PRIVATE kiko_core ftxui::component)
add_test(NAME tui_menu_state COMMAND kiko_tui_menu_state_test)

add_executable(kiko_tui_menu_view_test tests/tui_menu_view_test.cpp src/tui/tui_menu_view.cpp
               src/tui/tui_menu_state.cpp src/tui/tui_transfer_view.cpp)
target_link_libraries(kiko_tui_menu_view_test PRIVATE kiko_core ftxui::component)
add_test(NAME tui_menu_view COMMAND kiko_tui_menu_view_test)

add_executable(kiko_tui_session_test tests/tui_session_test.cpp src/tui/tui_session.cpp src/tui/tui_transfer_view.cpp)
target_link_libraries(kiko_tui_session_test PRIVATE kiko_core ftxui::component)
add_test(NAME tui_session COMMAND kiko_tui_session_test)

add_executable(kiko_tui_failure_hint_test tests/tui_failure_hint_test.cpp src/tui/tui_failure_hint.cpp)
target_link_libraries(kiko_tui_failure_hint_test PRIVATE kiko_core)
add_test(NAME tui_failure_hint COMMAND kiko_tui_failure_hint_test)

add_executable(kiko_tui_browser_test tests/tui_browser_test.cpp src/tui/tui_browser.cpp)
target_link_libraries(kiko_tui_browser_test PRIVATE kiko_core ftxui::component)
add_test(NAME tui_browser COMMAND kiko_tui_browser_test)

add_executable(kiko_web_test tests/web_test.cpp src/web/web.cpp src/web/web_assets.cpp src/web/web_job.cpp)
target_link_libraries(kiko_web_test PRIVATE kiko_core)
add_test(NAME web COMMAND kiko_web_test)
add_executable(kiko_web_job_test tests/web_job_test.cpp src/web/web_job.cpp)
target_link_libraries(kiko_web_job_test PRIVATE kiko_core)
add_test(NAME web_job COMMAND kiko_web_job_test)
kiko_add_core_test(note_protocol tests/note_protocol_test.cpp)
kiko_add_core_test(note_workspace tests/note_workspace_test.cpp)
kiko_add_core_test(mux tests/mux_test.cpp)
kiko_add_core_test(adaptive tests/adaptive_test.cpp)
kiko_add_core_test(network_interfaces tests/network_interfaces_test.cpp)
kiko_add_core_test(connectivity tests/connectivity_test.cpp)
kiko_add_core_test(peer_session tests/peer_session_test.cpp)
kiko_add_core_test(route_session tests/route_session_test.cpp)
kiko_add_core_test(route_scenarios tests/route_scenarios_test.cpp)
kiko_add_core_test(outbound_policy tests/outbound_policy_test.cpp)
kiko_add_core_test(relay_room_state tests/relay_room_state_test.cpp)
kiko_add_core_test(relay_route_state tests/relay_route_state_test.cpp)
kiko_add_core_test(relay_protocol tests/relay_protocol_test.cpp)
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
  add_test(NAME web_smoke COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/web_smoke.py $<TARGET_FILE:kiko>)
  set_tests_properties(web_smoke PROPERTIES DEPENDS kiko TIMEOUT 20)
  add_test(NAME web_note_smoke COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/web_note_smoke.py
           $<TARGET_FILE:kiko> $<TARGET_FILE:kiko-relayd>)
  set_tests_properties(web_note_smoke PROPERTIES DEPENDS "kiko;kiko-relayd" TIMEOUT 30)
  add_test(NAME pair_timeout_smoke COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/pair_timeout_smoke.py
           $<TARGET_FILE:kiko> $<TARGET_FILE:kiko-relayd>)
  set_tests_properties(pair_timeout_smoke PROPERTIES DEPENDS "kiko;kiko-relayd" TIMEOUT 12)
  add_test(NAME web_browser_smoke COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/web_browser_smoke.py $<TARGET_FILE:kiko>)
  set_tests_properties(web_browser_smoke PROPERTIES DEPENDS kiko TIMEOUT 30 SKIP_RETURN_CODE 77)
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
