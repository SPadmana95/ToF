cmake_minimum_required(VERSION 3.10)
project(fw_upgrade)

set(NVM_COMMON ../common)

set(SOURCES
${NVM_COMMON}/nvm_tools_common.cpp
)

add_executable(${PROJECT_NAME} fw_upgrade.cpp ${SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE ${NVM_COMMON})
target_link_libraries(${PROJECT_NAME} PRIVATE crc32)
