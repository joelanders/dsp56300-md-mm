cmake_minimum_required(VERSION 3.10)

project(dsp56kTestRunner)

add_executable(dsp56kTestRunner)

target_sources(dsp56kTestRunner PRIVATE testRunner.cpp)

target_link_libraries(dsp56kTestRunner PUBLIC dsp56kEmu)

add_test(NAME dsp56300_unitTests COMMAND dsp56kTestRunner)
set_tests_properties(dsp56300_unitTests PROPERTIES LABELS "UnitTest;dsp56300")

add_executable(dspDmaStatusTest dmaStatusTest.cpp)
target_link_libraries(dspDmaStatusTest PRIVATE dsp56kEmu)
add_test(NAME dsp56300_dmaStatus COMMAND dspDmaStatusTest)
set_tests_properties(dsp56300_dmaStatus PROPERTIES LABELS "UnitTest;dsp56300;DMA" TIMEOUT 10)
set_property(TARGET dspDmaStatusTest PROPERTY FOLDER "dsp56300")
