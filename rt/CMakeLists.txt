cmake_minimum_required(VERSION 3.28)
project(ecat_rasp4 LANGUAGES C)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/SOEM soem EXCLUDE_FROM_ALL)

add_executable(ecat-scan tools/ecat-scan.c)
target_link_libraries(ecat-scan soem)
