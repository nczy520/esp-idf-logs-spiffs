cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS ../../esp_logs_spiffs)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(logs_spiffs_example)
