# Unit tests compile the analyzer sources against an in-process mock of the
# Analyzer SDK runtime (mock_sdk.cpp) instead of linking Saleae::AnalyzerSDK,
# so the decoder and simulation generator can run inside a plain executable.
add_executable(qspi_tests
    mock_sdk.cpp
    qspi_tests.cpp
    ${CMAKE_SOURCE_DIR}/src/QuadSpiAnalyzer.cpp
    ${CMAKE_SOURCE_DIR}/src/QuadSpiAnalyzerSettings.cpp
    ${CMAKE_SOURCE_DIR}/src/QuadSpiAnalyzerResults.cpp
    ${CMAKE_SOURCE_DIR}/src/QuadSpiSimulationDataGenerator.cpp
)
target_include_directories(qspi_tests PRIVATE ${analyzersdk_SOURCE_DIR}/include)

add_test(NAME qspi_decoder_tests COMMAND qspi_tests)

# QSPI_COVERAGE=ON instruments the test executable (which compiles the analyzer
# sources directly) for the CI coverage gate. Clang's source-based coverage is
# preferred: it counts only source-level branch conditions, with none of gcov's
# exception-unwind branch artifacts.
option(QSPI_COVERAGE "Instrument the unit tests for coverage" OFF)
if(QSPI_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(qspi_tests PRIVATE -fprofile-instr-generate -fcoverage-mapping -O0 -g)
        target_link_options(qspi_tests PRIVATE -fprofile-instr-generate)
    else()
        target_compile_options(qspi_tests PRIVATE --coverage -O0 -g)
        target_link_options(qspi_tests PRIVATE --coverage)
    endif()
endif()
