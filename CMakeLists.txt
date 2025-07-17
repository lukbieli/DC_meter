# The following four lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

set(MAIN_SRCS main/main.c)

# Use environment variable if set, otherwise fallback to default
if(DEFINED ENV{ESP_IDF_LIB_PATH})
    set(EXTRA_COMPONENT_DIRS $ENV{ESP_IDF_LIB_PATH})
else()
    set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../esp-idf-lib/components")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(example-ina219)
