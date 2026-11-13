cmake_minimum_required(VERSION 3.13)

# Dummy "machine" for the Universal Black Box: an unmodified RP2040 target
# (PicoBricks / Pico) that runs a small state machine and eventually crashes.
set(PICO_BOARD pico CACHE STRING "Board type")

include(pico_sdk_import.cmake)
project(ubb_target_demo C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(ubb_target_demo src/main.c)
target_link_libraries(ubb_target_demo pico_stdlib hardware_pwm hardware_adc hardware_watchdog)
# Release optimisation + DWARF, exactly what the black box needs from a PlatformIO build (-g changes no code).
target_compile_options(ubb_target_demo PRIVATE -O2 -g)
pico_enable_stdio_usb(ubb_target_demo 0)
pico_enable_stdio_uart(ubb_target_demo 1)
pico_add_extra_outputs(ubb_target_demo)
