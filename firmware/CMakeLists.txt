cmake_minimum_required(VERSION 3.13)

# Universal Black Box - probe firmware for Raspberry Pi Pico 2 W
set(PICO_BOARD pico2_w CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")

include(pico_sdk_import.cmake)

project(ubb_probe C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(ubb_probe
    src/main.c
    src/swd.c
    src/dap.c
    src/crc.c
    src/stream.c
    src/flashlog.c
    src/manifest.c
    src/sampler.c
    src/host.c
)

pico_generate_pio_header(ubb_probe ${CMAKE_CURRENT_LIST_DIR}/src/swd.pio)

target_include_directories(ubb_probe PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_options(ubb_probe PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(ubb_probe
    pico_stdlib
    pico_multicore
    pico_unique_id
    hardware_pio
    hardware_flash
    hardware_sync
    hardware_gpio
)

# USB CDC for the host tool, no UART console
pico_enable_stdio_usb(ubb_probe 1)
pico_enable_stdio_uart(ubb_probe 0)

# Run entirely from SRAM so flash writes on core 1 never stall the recorder on core 0.
pico_set_binary_type(ubb_probe copy_to_ram)

pico_set_program_name(ubb_probe "Universal Black Box probe")
pico_set_program_description(ubb_probe "SWD flight recorder for RP2040/RP2350 targets")

pico_add_extra_outputs(ubb_probe)
