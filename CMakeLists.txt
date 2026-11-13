cmake_minimum_required(VERSION 3.16)
project(high_dpi_scaling VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Final packaged Kit output folder and archive paths
set(KIT_DIST_DIR "${CMAKE_SOURCE_DIR}/dist")
set(KIT_OUTPUT_DIR "${KIT_DIST_DIR}/high_dpi_scaling")
set(KIT_ZIP_FILE "${KIT_DIST_DIR}/high_dpi_scaling.zip")
set(KIT_LPK_FILE "${KIT_DIST_DIR}/high_dpi_scaling.lpk")
set(KIT_RESOURCES_DIR "${CMAKE_SOURCE_DIR}/resources")

add_library(high_dpi_scaling SHARED
    src/high_dpi_scaling.cpp
)

set_target_properties(high_dpi_scaling PROPERTIES
    OUTPUT_NAME "high_dpi_scaling"
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)

if(MSVC)
    target_compile_options(high_dpi_scaling PRIVATE /W4 /EHsc)
endif()

# Package the compiled .dll, the Python lxserv plugin from src/, and static
# resources from resources/ into a clean dist/ directory (.zip and .lpk).
add_custom_command(TARGET high_dpi_scaling POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${KIT_DIST_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${KIT_OUTPUT_DIR}/lxserv"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:high_dpi_scaling>" "${KIT_OUTPUT_DIR}/"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/src/high_dpi_scaling_plugin.py" "${KIT_OUTPUT_DIR}/lxserv/"
    COMMAND "${CMAKE_COMMAND}" -E copy "${KIT_RESOURCES_DIR}/index.cfg" "${KIT_OUTPUT_DIR}/"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${KIT_DIST_DIR}" "${CMAKE_COMMAND}" -E tar "cf" "${KIT_ZIP_FILE}" --format=zip high_dpi_scaling
    COMMAND "${CMAKE_COMMAND}" -E copy "${KIT_RESOURCES_DIR}/index.xml" "${KIT_OUTPUT_DIR}/"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${KIT_OUTPUT_DIR}" "${CMAKE_COMMAND}" -E tar "cf" "${KIT_LPK_FILE}" --format=zip index.xml index.cfg high_dpi_scaling.dll lxserv/high_dpi_scaling_plugin.py
    COMMAND "${CMAKE_COMMAND}" -E remove -f "${KIT_OUTPUT_DIR}/index.xml"
    COMMENT "Packaging high_dpi_scaling Kit into dist/high_dpi_scaling/, .zip, and .lpk..."
)
