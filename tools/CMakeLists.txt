cmake_minimum_required(VERSION 3.10)
project(tools)

if (ON_TARGET)
    add_subdirectory(nvm_tools)
endif()

include(GNUInstallDirs)
