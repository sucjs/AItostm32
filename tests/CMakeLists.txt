cmake_minimum_required(VERSION 3.22)

#
# 宿主机单元测试(不交叉编译, 不依赖 HAL)
#
# 只覆盖纯逻辑模块: 这些模块不引用任何 STM32 头文件, 因此可以在 PC 上
# 直接编译运行, 用于快速验证协议解析/编码与日志环形缓冲的正确性.
#
# 用法:
#   cmake -S tests -B build/tests
#   cmake --build build/tests
#   ctest --test-dir build/tests --output-on-failure
# 或直接使用仓库根的 scripts/run_host_tests.ps1
#
project(AItostm32_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

enable_testing()

set(CORE_INC ${CMAKE_CURRENT_SOURCE_DIR}/../Core/Inc)
set(CORE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../Core/Src)

# 被测的纯逻辑源码(与固件共用同一份文件, 避免测试与实现漂移)
set(HOST_LOGIC_SRC
    ${CORE_SRC}/signal_protocol.c
    ${CORE_SRC}/signal_log.c
    ${CORE_SRC}/knob.c
    ${CORE_SRC}/button.c
)

add_executable(test_signal_protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/test_signal_protocol.c
    ${CORE_SRC}/signal_protocol.c
)
target_include_directories(test_signal_protocol PRIVATE ${CORE_INC})
target_compile_options(test_signal_protocol PRIVATE -Wall -Wextra)
add_test(NAME signal_protocol COMMAND test_signal_protocol)

add_executable(test_signal_log
    ${CMAKE_CURRENT_SOURCE_DIR}/test_signal_log.c
    ${CORE_SRC}/signal_log.c
)
target_include_directories(test_signal_log PRIVATE ${CORE_INC})
target_compile_options(test_signal_log PRIVATE -Wall -Wextra)
add_test(NAME signal_log COMMAND test_signal_log)

add_executable(test_button
    ${CMAKE_CURRENT_SOURCE_DIR}/test_button.c
    ${CORE_SRC}/button.c
)
target_include_directories(test_button PRIVATE ${CORE_INC})
target_compile_options(test_button PRIVATE -Wall -Wextra)
add_test(NAME button COMMAND test_button)

add_executable(test_knob
    ${CMAKE_CURRENT_SOURCE_DIR}/test_knob.c
    ${CORE_SRC}/knob.c
)
target_include_directories(test_knob PRIVATE ${CORE_INC})
target_compile_options(test_knob PRIVATE -Wall -Wextra)
add_test(NAME knob COMMAND test_knob)

# 便于 IDE/编辑器索引全部纯逻辑源码
add_library(host_logic OBJECT ${HOST_LOGIC_SRC})
target_include_directories(host_logic PRIVATE ${CORE_INC})
