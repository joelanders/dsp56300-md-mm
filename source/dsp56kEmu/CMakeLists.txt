cmake_minimum_required(VERSION 3.10)
project(dsp56kEmu)

add_library(dsp56kEmu)

set(SOURCES
aar.h
agu.cpp agu.h
assembler.cpp assembler.h
assemblertest.cpp assemblertest.h
audio.cpp audio.h
debuggerinterface.cpp debuggerinterface.h
disasm.cpp disasm.h
dma.cpp dma.h
dspconfig.h
dsplistener.h
dspregs.h
dsp.cpp dsp.h 
dsp_decode.inl
dsp_jumptable.inl
dsp_ops.inl dsp_ops_helper.inl 
dsp_ops_alu.inl dsp_ops_bra.inl dsp_ops_jmp.inl dsp_ops_move.inl
dspthread.cpp dspthread.h
dspBootCode.cpp dspBootCode.h
error.cpp error.h
esai.cpp esai.h
esaiclock.cpp esaiclock.h
essi.cpp essi.h
esxi.cpp esxi.h
gpio.cpp gpio.h
hdi08.cpp hdi08.h
hdi08queue.cpp hdi08queue.h
haltDSP.cpp haltDSP.h
hi08.h
instructioncache.cpp instructioncache.h
interpreterunittests.cpp interpreterunittests.h
memory.cpp memory.h
interrupts.h
memorybuffer.cpp memorybuffer.h
omfloader.cpp omfloader.h
opcodes.cpp opcodes.h
opcodeanalysis.h
opcodecycles.cpp opcodecycles.h
opcodefields.h
opcodeinfo.h
opcodetypes.h
peripherals.cpp peripherals.h
registers.cpp registers.h
timers.cpp timers.h
types.cpp types.h
unittests.cpp unittests.h
utils.cpp utils.h
)

set(SOURCES_JIT
jit.cpp jit.h
jitasmjithelpers.cpp jitasmjithelpers.h
jitconfig.h
jitdspmode.cpp jitdspmode.h
jitemitter.cpp jitemitter.h
jitblock.cpp jitblock.h
jitblockchain.cpp jitblockchain.h
jitblockemitter.h
jitblockinfo.cpp jitblockinfo.h
jitblockruntimedata.cpp jitblockruntimedata.h
jitcacheentry.h
jithelper.cpp jithelper.h
jitdspregs.cpp jitdspregs.h
jitdspregpool.cpp jitdspregpool.h
jitdspregpoolregpair.cpp jitdspregpoolregpair.h
jitdspregpooltypes.h
jitdspvalue.cpp jitdspvalue.h
jitmem.cpp jitmem.h
jitops.cpp jitops.h jitops_agu.cpp jitops_alu.cpp jitops_ccr.cpp jitops_decode.cpp jitops_helper.cpp jitops_jmp.cpp jitops_mem.cpp jitops_move.cpp
jitops_alu.inl jitops_helper.inl jitops_jmp.inl jitops_mem.inl jitops_move.inl
jitoptimizer.cpp jitoptimizer.h
jitoptimizertests.cpp jitoptimizertests.h
jitprofilingsupport.cpp jitprofilingsupport.h
jitregtracker.cpp jitregtracker.h
jitregtypes.h
jitruntimedata.cpp jitruntimedata.h
jitstackhelper.cpp jitstackhelper.h
jittypes.h
jittrampoline.cpp jittrampoline.h
jitunittests.cpp jitunittests.h

jitops_agu_aarch64.cpp
jitops_alu_aarch64.cpp
jitops_ccr_aarch64.cpp
jitops_decode_aarch64.cpp
jitops_helper_aarch64.cpp
jitops_jmp_aarch64.inl

jitops_agu_x64.cpp
jitops_alu_x64.cpp
jitops_ccr_x64.cpp
jitops_decode_x64.cpp
jitops_helper_x64.cpp
jitops_jmp_x64.inl
)

target_sources(dsp56kEmu PRIVATE ${SOURCES} ${SOURCES_JIT} ${SOURCES_VTUNE})
source_group("source" FILES ${SOURCES})
source_group("source\\jit" FILES ${SOURCES_JIT})

if(WIN32 OR (UNIX AND NOT APPLE))
	target_link_libraries(dsp56kEmu PUBLIC vtuneSdk)
	target_compile_definitions(dsp56kEmu PRIVATE DSP56K_USE_VTUNE_JIT_PROFILING_API)
endif()

if(MSVC)
	SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /constexpr:steps10000000")
elseif(UNIX AND NOT APPLE)
	if(NOT ANDROID)
		SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fconstexpr-loop-limit=10000000")
	endif()
endif()

if(UNIX AND NOT APPLE)
	set(THREADS_PREFER_PTHREAD_FLAG ON)
	find_package(Threads REQUIRED)
	target_link_libraries(dsp56kEmu PUBLIC Threads::Threads)
	target_compile_definitions(dsp56kEmu PRIVATE DSP56K_USE_PERF_JIT_PROFILING)
endif()

target_link_libraries(dsp56kEmu PUBLIC dsp56kBase asmjit)

if(DSP56300_DEBUGGER)
	target_compile_definitions(dsp56kEmu PUBLIC DSP56300_DEBUGGER=1)
else()
	target_compile_definitions(dsp56kEmu PUBLIC DSP56300_DEBUGGER=0)
endif()

# This should use ${PROJECT_SOURCE_DIR}/source if the top level of the repo
# has the top level cmakelists.txt file.
target_include_directories(dsp56kEmu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(BUILD_TESTING)
	add_executable(dsp56kEssiTransportTest essi_transport_test.cpp)
	target_link_libraries(dsp56kEssiTransportTest PRIVATE dsp56kEmu)
	add_test(NAME dsp56300_essi_transport COMMAND dsp56kEssiTransportTest)
	set_tests_properties(dsp56300_essi_transport PROPERTIES TIMEOUT 10)

	add_executable(dsp56kDspBootTest dspboot_foundation_test.cpp)
	target_link_libraries(dsp56kDspBootTest PRIVATE dsp56kEmu)
	target_compile_features(dsp56kDspBootTest PRIVATE cxx_std_17)
	add_test(NAME dsp56300_dspboot_foundation COMMAND dsp56kDspBootTest)
	set_tests_properties(dsp56300_dspboot_foundation PROPERTIES TIMEOUT 10)

	add_executable(dsp56kDynamicFastInterruptTest dynamic_fast_interrupt_test.cpp)
	target_link_libraries(dsp56kDynamicFastInterruptTest PRIVATE dsp56kEmu)
	target_compile_features(dsp56kDynamicFastInterruptTest PRIVATE cxx_std_17)
	add_test(NAME dsp56300_dynamic_fast_interrupt COMMAND dsp56kDynamicFastInterruptTest)
	set_tests_properties(dsp56300_dynamic_fast_interrupt PROPERTIES TIMEOUT 10)

	add_executable(dsp56kEssiGpioTest essi_gpio_test.cpp)
	target_link_libraries(dsp56kEssiGpioTest PRIVATE dsp56kEmu)
	target_compile_features(dsp56kEssiGpioTest PRIVATE cxx_std_17)
	add_test(NAME dsp56300_essi_gpio COMMAND dsp56kEssiGpioTest)
	set_tests_properties(dsp56300_essi_gpio PROPERTIES TIMEOUT 10)

	add_executable(dsp56kEssiClockGateTest essi_clock_gate_test.cpp)
	target_link_libraries(dsp56kEssiClockGateTest PRIVATE dsp56kEmu)
	target_compile_features(dsp56kEssiClockGateTest PRIVATE cxx_std_17)
	add_test(NAME dsp56300_essi_clock_gate COMMAND dsp56kEssiClockGateTest)
	set_tests_properties(dsp56300_essi_clock_gate PROPERTIES TIMEOUT 10)

	add_executable(dsp56kMaskedInterruptPeripheralsTest masked_interrupt_peripherals_test.cpp)
	target_link_libraries(dsp56kMaskedInterruptPeripheralsTest PRIVATE dsp56kEmu)
	target_compile_features(dsp56kMaskedInterruptPeripheralsTest PRIVATE cxx_std_17)
	add_test(NAME dsp56300_masked_interrupt_peripherals COMMAND dsp56kMaskedInterruptPeripheralsTest)
	set_tests_properties(dsp56300_masked_interrupt_peripherals PROPERTIES TIMEOUT 10)

	add_executable(dsp56kBudgetedExecutionTest budgeted_execution_test.cpp)
	target_link_libraries(dsp56kBudgetedExecutionTest PRIVATE dsp56kEmu)
	target_compile_features(dsp56kBudgetedExecutionTest PRIVATE cxx_std_17)
	add_test(NAME dsp56300_budgeted_execution COMMAND dsp56kBudgetedExecutionTest)
	set_tests_properties(dsp56300_budgeted_execution PROPERTIES TIMEOUT 10)

	add_executable(dsp56kJitCycleParityTest jit_cycle_parity_test.cpp)
	target_link_libraries(dsp56kJitCycleParityTest PRIVATE dsp56kEmu)
	target_compile_features(dsp56kJitCycleParityTest PRIVATE cxx_std_17)
	add_test(NAME dsp56300_jit_cycle_parity COMMAND dsp56kJitCycleParityTest)
	set_tests_properties(dsp56300_jit_cycle_parity PROPERTIES TIMEOUT 10)
endif()
