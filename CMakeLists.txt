cmake_minimum_required (VERSION 3.13)

project(QuadSpiAnalyzer)

add_definitions( -DLOGIC2 )

set(CMAKE_OSX_DEPLOYMENT_TARGET "10.14" CACHE STRING "Minimum supported MacOS version" FORCE)

# enable generation of compile_commands.json, helpful for IDEs to locate include files.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# custom CMake Modules are located in the cmake directory.
set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

include(ExternalAnalyzerSDK)

set(SOURCES 
src/QuadSpiAnalyzer.cpp
src/QuadSpiAnalyzer.h
src/QuadSpiAnalyzerResults.cpp
src/QuadSpiAnalyzerResults.h
src/QuadSpiAnalyzerSettings.cpp
src/QuadSpiAnalyzerSettings.h
src/QuadSpiSimulationDataGenerator.cpp
src/QuadSpiSimulationDataGenerator.h
)

add_analyzer_plugin(${PROJECT_NAME} SOURCES ${SOURCES})

option(QSPI_BUILD_TESTS "Build the QSPI analyzer unit tests" ON)
if(QSPI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
