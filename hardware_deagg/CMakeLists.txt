cmake_minimum_required(VERSION 3.13)

# Pull in the Pico SDK (must be set before project())
# Either set PICO_SDK_PATH env var or pass -DPICO_SDK_PATH=... to cmake
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(pico_vedirect C CXX ASM)
set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pico_vedirect
	main.c
	router.c
	cluster.c
	usb_descriptors.c
)

# Node ID: 0-7, set via -DNODE_ID=N at cmake time or leave at 0
if(NOT DEFINED NODE_ID)
	set(NODE_ID 0)
endif()
target_compile_definitions(pico_vedirect PRIVATE
	NODE_ID=${NODE_ID}
)

target_include_directories(pico_vedirect PRIVATE
	${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(pico_vedirect
	pico_stdlib
	pico_unique_id
	tinyusb_device
	tinyusb_board
	hardware_uart
	hardware_gpio
	hardware_irq
	hardware_timer
)

# Generate UF2 flash image and map/dis files
pico_add_extra_outputs(pico_vedirect)

# Optional: USB stdio disabled (we own USB for CDC)
pico_enable_stdio_usb(pico_vedirect 0)
pico_enable_stdio_uart(pico_vedirect 0)
