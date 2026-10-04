# 示例程序：每个 .c 编译为一个可执行文件，输出到 build/bin
set(MB_EXAMPLES
    01_basic_pubsub
    02_wildcards_filter
    03_lvgl_motor_sensor
    04_node_events)

foreach(example IN LISTS MB_EXAMPLES)
    add_executable(example_${example} ${example}.c)
    target_link_libraries(example_${example} PRIVATE message_bus::message_bus mb_warnings)
    set_target_properties(example_${example} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()
