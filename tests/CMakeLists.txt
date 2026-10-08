# 单元测试：单个可执行文件 + CTest 注册
add_executable(mb_tests
    test_main.c
    test_topic.c
    test_bus.c
    test_pubsub.c
    test_threads.c
    test_async.c)

target_link_libraries(mb_tests PRIVATE message_bus::message_bus mb_warnings)

if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(mb_tests PRIVATE Threads::Threads)
endif()

set_target_properties(mb_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_test(NAME mb_tests COMMAND mb_tests)

# 输出里出现 [FAIL] 标记时也判为失败（与进程退出码互为双保险）。
# 注意：方括号必须转义成 \\[ ... \\]，否则 CMake 交给正则的是字符类 [FAIL]，
# 那会匹配任意一个 F/A/I/L 字母 —— 连 "ALL PASSED" 都会被误判成失败。
set_tests_properties(mb_tests PROPERTIES
    FAIL_REGULAR_EXPRESSION "\\[FAIL\\]"
    TIMEOUT 60)
