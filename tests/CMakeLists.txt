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
