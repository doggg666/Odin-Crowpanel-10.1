cmake_minimum_required(VERSION 3.16)

# Odin firmware project for Elecrow CrowPanel Advanced 10.1 V1.0.
# Build with the matching Elecrow V1.0 BSP added during the hardware
# integration milestone. This starter deliberately does not guess board
# initialization, display, touch, camera, audio, or ESP32-C6 transport APIs.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(odin_crowpanel_v1_0)
