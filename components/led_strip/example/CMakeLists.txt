# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.20)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# add the component directories that we want to use
set(EXTRA_COMPONENT_DIRS
  "../../../components/"
)

set(
  COMPONENTS
  "main esptool_py driver logger task rmt led_strip"
  CACHE STRING
  "List of components to include"
  )

project(led_strip_example)

set(CMAKE_CXX_STANDARD 20)
