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

set(SHARED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../shared)

add_executable(ecat-rt src/main.c src/bus.c src/config.c src/cia402.c src/profile.c src/ipc.c)
target_include_directories(ecat-rt PRIVATE ${SHARED_DIR})
target_link_libraries(ecat-rt soem pthread m rt)

add_executable(ecat-ctl tools/ecat-ctl.c)
target_include_directories(ecat-ctl PRIVATE ${SHARED_DIR})
target_link_libraries(ecat-ctl rt)

add_executable(ipc-layout-check tools/ipc-layout-check.c)
target_include_directories(ipc-layout-check PRIVATE ${SHARED_DIR})
