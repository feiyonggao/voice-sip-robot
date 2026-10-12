cmake_minimum_required(VERSION 3.28)
project(voicebot VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/utf-8)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network WebSockets Widgets Multimedia)

# Keep the application build separate from PJPROJECT's existing applications.
if(NOT TARGET pjsua2)
  add_compile_definitions(PJSUA_MAX_CALLS=180)
  set(BUILD_TESTING OFF CACHE BOOL "Build PJPROJECT tests")
  set(PJMEDIA_WITH_VIDEO OFF CACHE BOOL "Build video support")
  set(PJMEDIA_WITH_AUDIODEV OFF CACHE BOOL "Build hardware audio backends")
  set(PJLIB_WITH_SSL "" CACHE STRING "SIP TLS backend (Qwen uses Qt TLS)")
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../.." pjproject EXCLUDE_FROM_ALL)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_library(voicebot-core STATIC
  bot_config.h bot_config.cpp
  bot_engine.h bot_engine.cpp
  call_record.h call_record.cpp
  file_logger.h file_logger.cpp
  local_audio.h local_audio.cpp
  mcp_client.h mcp_client.cpp
  pcm_bridge.h pcm_bridge.cpp
  qwen_session.h qwen_session.cpp
  tool_registry.h tool_registry.cpp
)
target_link_libraries(voicebot-core PUBLIC Qt6::Core Qt6::Network Qt6::WebSockets Qt6::Multimedia
  pjsua2 pjsua-lib pjsip pjmedia pjlib pjlib-util pjnath)
target_include_directories(voicebot-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# Windows 下把应用图标嵌入 exe 资源（任务栏/资源管理器显示）
if(WIN32)
  set(VOICEBOT_RC assets/voicebot.rc)
endif()
qt_add_executable(voicebot main.cpp main_window.h main_window.cpp
  assets/icons.qrc ${VOICEBOT_RC})
# 构建后把插件目录拷到可执行文件旁（../plugins），开发运行时即可相对路径定位；
# Windows: build/Release/voicebot.exe -> build/plugins；Linux: build/voicebot -> ../plugins
add_custom_command(TARGET voicebot POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/plugins"
          "$<TARGET_FILE_DIR:voicebot>/../plugins")
set_target_properties(voicebot PROPERTIES WIN32_EXECUTABLE TRUE)
target_compile_definitions(voicebot PRIVATE VOICEBOT_VERSION="${PROJECT_VERSION}")
target_link_libraries(voicebot PRIVATE voicebot-core Qt6::Widgets)
if(MSVC)
  target_compile_options(voicebot-core PRIVATE /utf-8 /W4)
  target_compile_options(voicebot PRIVATE /utf-8 /W4)
endif()
install(TARGETS voicebot RUNTIME DESTINATION bin BUNDLE DESTINATION .)
# 插件目录与产物打包：<prefix>/plugins（与 bin 平级），程序按相对路径定位
install(DIRECTORY plugins/ DESTINATION plugins)
# 默认配置模板（带注释），打包到 <prefix>/voicebot.ini.default，用户按需修改后通过 --config 指定
install(FILES voicebot.ini.default DESTINATION .)
#qt_generate_deploy_app_script 需要 Qt 6.5+，Windows 下用于 windeployqt 打包 DLL
#Linux 下使用系统 Qt 库，无需部署脚本
if(Qt6_VERSION VERSION_GREATER_EQUAL "6.5")
  qt_generate_deploy_app_script(TARGET voicebot OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR)
  install(SCRIPT ${deploy_script})
endif()

# ---- 安装包打包（CPack，复用上述 install 规则）----
# Windows：ZIP 随时可打；安装 NSIS 后 `cpack -G NSIS` 生成 .exe 向导安装包
# Linux：TGZ 压缩包
set(CPACK_PACKAGE_NAME "voicebot")
set(CPACK_PACKAGE_VENDOR "voicebot")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Voice SIP Robot - AI 电话语音机器人")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "voicebot")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_LIST_DIR}/../../../COPYING")
set(CPACK_OUTPUT_FILE_PREFIX "${CMAKE_BINARY_DIR}/packages")
if(WIN32)
  set(CPACK_GENERATOR "ZIP")
  set(CPACK_PACKAGE_EXECUTABLES "voicebot" "Voice SIP Robot")
  set(CPACK_NSIS_DISPLAY_NAME "Voice SIP Robot ${PROJECT_VERSION}")
  set(CPACK_NSIS_PACKAGE_NAME "Voice SIP Robot")
  set(CPACK_NSIS_MUI_ICON "${CMAKE_CURRENT_LIST_DIR}/assets/voicebot.ico")
  set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\voicebot.exe")
else()
  set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)
