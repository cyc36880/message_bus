cmake_minimum_required(VERSION 3.16)

project(message_bus
    VERSION 1.1.0
    DESCRIPTION "In-process MQTT-style message bus for decoupling LVGL from hardware"
    LANGUAGES C)

# ---------------------------------------------------------------------------
# 选项
# ---------------------------------------------------------------------------
option(MB_BUILD_EXAMPLES "build the example programs" ON)
option(MB_BUILD_TESTS "build the unit tests" ON)
option(MB_BUILD_SHARED "build a shared library (static by default)" OFF)
option(MB_WARNINGS_AS_ERRORS "treat warnings as errors" OFF)

set(MB_OS "auto" CACHE STRING "OS abstraction layer: auto | none | freertos | posix | win32")
set_property(CACHE MB_OS PROPERTY STRINGS auto none freertos posix win32)

set(MB_FREERTOS_INCLUDE_DIR "" CACHE PATH "directory containing FreeRTOS.h (needed when MB_OS=freertos)")

# ---------------------------------------------------------------------------
# 解析 MB_OS
# ---------------------------------------------------------------------------
if(MB_OS STREQUAL "auto")
    if(WIN32)
        set(MB_OS_RESOLVED "win32")
    else()
        set(MB_OS_RESOLVED "posix")
    endif()
else()
    set(MB_OS_RESOLVED "${MB_OS}")
endif()

if(MB_OS_RESOLVED STREQUAL "win32")
    set(MB_OS_DEFINE "MB_OS_WIN32")
    set(MB_OS_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/port/mb_os_win32.c")
elseif(MB_OS_RESOLVED STREQUAL "posix")
    set(MB_OS_DEFINE "MB_OS_POSIX")
    set(MB_OS_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/port/mb_os_posix.c")
    find_package(Threads REQUIRED)
elseif(MB_OS_RESOLVED STREQUAL "freertos")
    set(MB_OS_DEFINE "MB_OS_FREERTOS")
    set(MB_OS_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/port/mb_os_freertos.c")
    if(NOT MB_FREERTOS_INCLUDE_DIR)
        message(WARNING
            "MB_OS=freertos was requested but MB_FREERTOS_INCLUDE_DIR is not set; "
            "point it at the directory containing FreeRTOS.h "
            "(e.g. .../FreeRTOS-Kernel/include)")
    endif()
elseif(MB_OS_RESOLVED STREQUAL "none")
    set(MB_OS_DEFINE "MB_OS_NONE")
    set(MB_OS_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/port/mb_os_none.c")
else()
    message(FATAL_ERROR "unknown MB_OS value: ${MB_OS} (expected auto|none|freertos|posix|win32)")
endif()

message(STATUS "message_bus: OS abstraction layer = ${MB_OS_RESOLVED} (${MB_OS_DEFINE})")

# ---------------------------------------------------------------------------
# 库目标
# ---------------------------------------------------------------------------
set(MB_SOURCES
    src/mb_async.c
    src/mb_bus.c
    src/mb_dispatch.c
    src/mb_log.c
    src/mb_message.c
    src/mb_node.c
    src/mb_subscription.c
    src/mb_topic.c
    ${MB_OS_SOURCE})

if(MB_BUILD_SHARED)
    add_library(message_bus SHARED ${MB_SOURCES})
else()
    add_library(message_bus STATIC ${MB_SOURCES})
endif()
add_library(message_bus::message_bus ALIAS message_bus)

target_include_directories(message_bus
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

# MB_CONFIG_OS 会影响公开头文件 mb_os.h 里的类型定义，因此必须是 PUBLIC 定义
target_compile_definitions(message_bus PUBLIC MB_CONFIG_OS=${MB_OS_DEFINE})

if(MB_FREERTOS_INCLUDE_DIR)
    target_include_directories(message_bus PUBLIC ${MB_FREERTOS_INCLUDE_DIR})
endif()

if(MB_OS_RESOLVED STREQUAL "posix")
    target_link_libraries(message_bus PUBLIC Threads::Threads)
endif()

set_target_properties(message_bus PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON          # gnu99：MinGW 的 windows.h 在严格 ANSI 下会缺声明
    POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# 告警：库、示例、测试统一使用同一套接口目标
# ---------------------------------------------------------------------------
if(MSVC)
    set(MB_WARNING_FLAGS /W4)
else()
    set(MB_WARNING_FLAGS -Wall -Wextra -Wpedantic)
    if(MB_WARNINGS_AS_ERRORS)
        list(APPEND MB_WARNING_FLAGS -Werror)
    endif()
endif()

# 库自身直接挂编译选项（不能用 PRIVATE 链接 INTERFACE 库，否则 install(EXPORT) 会报错）
target_compile_options(message_bus PRIVATE ${MB_WARNING_FLAGS})

# 示例与测试通过接口目标复用同一套告警设置
add_library(mb_warnings INTERFACE)
target_compile_options(mb_warnings INTERFACE ${MB_WARNING_FLAGS})

# ---------------------------------------------------------------------------
# 示例与测试
# ---------------------------------------------------------------------------
if(MB_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(MB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# 安装与导出（find_package(message_bus)）
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS message_bus
        EXPORT message_busTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/message_bus
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(FILES config/mb_conf_template.h
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/config)

install(FILES README.md CHANGELOG.md CONTRIBUTING.md LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

install(DIRECTORY docs/
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs
        FILES_MATCHING PATTERN "*.md")

install(EXPORT message_busTargets
        FILE message_busTargets.cmake
        NAMESPACE message_bus::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/message_bus)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/message_busConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/message_busConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/message_bus)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/message_busConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/message_busConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/message_busConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/message_bus)
