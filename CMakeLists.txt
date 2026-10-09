cmake_minimum_required(VERSION 3.16)
project(motor_de_regex VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# A biblioteca: inclua rx.h e ligue com rx.
add_library(rx STATIC src/utf8.c src/rx_parse.c src/rx_compile.c src/rx_vm.c)
target_include_directories(rx PUBLIC include PRIVATE src)

add_executable(rgrep src/rgrep.c)
target_include_directories(rgrep PRIVATE src)
target_link_libraries(rgrep PRIVATE rx)

enable_testing()
add_executable(test_rx tests/test_rx.c)
target_include_directories(test_rx PRIVATE src)
target_link_libraries(test_rx PRIVATE rx)
add_executable(rx_teste tests/rx_teste.c)
target_link_libraries(rx_teste PRIVATE rx)

add_test(NAME motor COMMAND test_rx)
add_test(NAME rgrep COMMAND sh ${CMAKE_SOURCE_DIR}/tests/rgrep.sh ${CMAKE_BINARY_DIR})
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME diferencial COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/diferencial.py $<TARGET_FILE:rx_teste>)
endif()
