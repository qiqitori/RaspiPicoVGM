cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(ym3012_dac C CXX ASM)

# if(NOT CMAKE_BUILD_TYPE)
#   set(CMAKE_BUILD_TYPE Release)
# endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
# set(CMAKE_CXX_FLAGS_RELEASE "-Os")
# set(CMAKE_C_FLAGS_RELEASE "-Os")

if (PICO_SDK_VERSION_STRING VERSION_LESS "1.3.0")
    message(FATAL_ERROR "Raspberry Pi Pico SDK version 1.3.0 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

# enable_language(C CXX ASM) # elimate error: CMake Error: Error required internal CMake variable not set, cmake may not be built correctly.  Missing variable is:  CMAKE_ASM_COMPILE_OBJECT

pico_sdk_init()

add_compile_options(-Wall -Wextra
        -Wno-format          # int != int32_t as far as the compiler is concerned because gcc has int32_t as long int
        -Wno-unused-function # we have some for the docs that aren't called
        -Wno-maybe-uninitialized
        -U__always_inline
        -U__force_inline
        )

add_executable(pico_opl3_player
  main.cpp
)

# target_include_directories(pico_opl3_player PUBLIC
#   ${CMAKE_CURRENT_LIST_DIR}
# )

target_link_libraries(pico_opl3_player PRIVATE
  pico_stdlib
  pico_multicore
#   hardware_pio
  hardware_pwm
)

target_sources(pico_opl3_player PRIVATE main.cpp)

#pico_generate_pio_header(pico_opl3_player ${CMAKE_CURRENT_LIST_DIR}/song_tick.pio)

pico_enable_stdio_usb(pico_opl3_player 1)
pico_enable_stdio_uart(pico_opl3_player 0)

pico_add_extra_outputs(pico_opl3_player)
