cmake_minimum_required(VERSION 3.16)
project(high_dpi_scaling VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Final packaged Kit output folder: dist/high_dpi_scaling
set(KIT_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/dist/high_dpi_scaling")
set(KIT_RESOURCES_DIR "${CMAKE_SOURCE_DIR}/resources")

add_library(high_dpi_scaling SHARED
    src/high_dpi_scaling.cpp
)

set_target_properties(high_dpi_scaling PROPERTIES
    OUTPUT_NAME "high_dpi_scaling"
    PREFIX ""
)

if(MSVC)
    target_compile_options(high_dpi_scaling PRIVATE /W4 /EHsc)
endif()

# Package the compiled .dll, the Python lxserv plugin from src/, and static
# resources from resources/ into dist/high_dpi_scaling/ after each build.
add_custom_command(TARGET high_dpi_scaling POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${KIT_OUTPUT_DIR}/lxserv"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:high_dpi_scaling>" "${KIT_OUTPUT_DIR}/"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_SOURCE_DIR}/src/high_dpi_scaling_plugin.py" "${KIT_OUTPUT_DIR}/lxserv/"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${KIT_RESOURCES_DIR}" "${KIT_OUTPUT_DIR}"
    COMMENT "Packaging high_dpi_scaling Kit into dist/high_dpi_scaling/..."
)
