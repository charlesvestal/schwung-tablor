cmake_minimum_required(VERSION 3.15)
project(schwung-tablor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
# No -ffast-math: it lets the compiler assume no NaN/Inf, which removes the
# safety of clamps on the realtime path (the ER-99 rule).
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

# Device runtime is glibc 2.35 / libstdc++ 6.0.29 — link the C++ runtime
# statically so the .so carries no libstdc++ version requirement at all.
set(TB_STATIC_LINK -static-libstdc++ -static-libgcc)

# The Move module: dlopen'd by Schwung's chain host as dsp.so
add_library(tablor SHARED
    src/dsp/tablor_plugin.cpp
    src/dsp/engine.cpp
    src/dsp/wt/loader.cpp
    src/ported/wavetable.cpp
    src/ported/audiofilter/ParametricCreator.cpp)
target_include_directories(tablor PRIVATE src src/dsp src/host src/ported)
target_link_libraries(tablor PRIVATE m pthread)
target_link_options(tablor PRIVATE ${TB_STATIC_LINK})
set_target_properties(tablor PROPERTIES OUTPUT_NAME "dsp" PREFIX "" SUFFIX ".so")
# glibc marks any DSO exporting an STB_GNU_UNIQUE symbol NODELETE, so dsp.so
# would never unload: a reload keeps running the old image and its memory is
# never freed. -fno-gnu-unique covers our own C++; --exclude-libs,ALL keeps
# the statically linked libstdc++'s symbols (std::string::npos etc.) local.
target_compile_options(tablor PRIVATE -fno-gnu-unique)
target_link_options(tablor PRIVATE -Wl,--exclude-libs,ALL)

# Loader test: dlopens the real dsp.so exactly like the chain host does.
# Cross-compiled and run ON the Move — the phase 0 gate.
add_executable(tablor_loadtest tools/loadtest.cpp)
target_include_directories(tablor_loadtest PRIVATE src src/dsp src/host)
target_link_libraries(tablor_loadtest PRIVATE dl m)
target_link_options(tablor_loadtest PRIVATE ${TB_STATIC_LINK})

# Phase 1 benchmark: mip build validation + realtime factor ON the Move.
# The go/no-go for the voice/unison budget.
add_executable(tablor_bench tools/bench.cpp src/ported/wavetable.cpp)
target_include_directories(tablor_bench PRIVATE src src/ported)
target_link_libraries(tablor_bench PRIVATE m)
target_link_options(tablor_bench PRIVATE ${TB_STATIC_LINK})

# Phase 2 DSP validation: numeric golden checks for every ported block.
# Built both natively (in-container, run at build time) and cross for the Move.
add_executable(tablor_test tests/test_dsp.cpp
               src/ported/wavetable.cpp
               src/ported/audiofilter/ParametricCreator.cpp)
target_include_directories(tablor_test PRIVATE src src/ported)
target_link_libraries(tablor_test PRIVATE m)
target_link_options(tablor_test PRIVATE ${TB_STATIC_LINK})

# Full-engine CPU check (run on the Move).
add_executable(tablor_enginebench tools/enginebench.cpp
               src/dsp/engine.cpp src/ported/wavetable.cpp
               src/ported/audiofilter/ParametricCreator.cpp)
target_include_directories(tablor_enginebench PRIVATE src src/ported src/dsp)
target_link_libraries(tablor_enginebench PRIVATE m)
target_link_options(tablor_enginebench PRIVATE ${TB_STATIC_LINK})

# Wavetable switch probe (run on the Move).
add_executable(tablor_hostlfo tools/hostlfo.cpp)
target_include_directories(tablor_hostlfo PRIVATE src src/host)
target_link_libraries(tablor_hostlfo PRIVATE dl m)
target_link_options(tablor_hostlfo PRIVATE ${TB_STATIC_LINK})

add_executable(tablor_dumpc tools/dumpc.cpp)
target_include_directories(tablor_dumpc PRIVATE src src/host)
target_link_libraries(tablor_dumpc PRIVATE dl m)
target_link_options(tablor_dumpc PRIVATE ${TB_STATIC_LINK})

add_executable(tablor_play tools/play.cpp)
target_include_directories(tablor_play PRIVATE src src/host)
target_link_libraries(tablor_play PRIVATE dl m)
target_link_options(tablor_play PRIVATE ${TB_STATIC_LINK})

add_executable(tablor_wtaudit tools/wtaudit.cpp)
target_include_directories(tablor_wtaudit PRIVATE src src/ported)
target_link_libraries(tablor_wtaudit PRIVATE m)
target_link_options(tablor_wtaudit PRIVATE ${TB_STATIC_LINK})

add_executable(tablor_render tools/render.cpp)
target_include_directories(tablor_render PRIVATE src src/host)
target_link_libraries(tablor_render PRIVATE dl m)
target_link_options(tablor_render PRIVATE ${TB_STATIC_LINK})

add_executable(tablor_quantprobe tools/quantprobe.cpp)
target_include_directories(tablor_quantprobe PRIVATE src src/host)
target_link_libraries(tablor_quantprobe PRIVATE dl m)
target_link_options(tablor_quantprobe PRIVATE ${TB_STATIC_LINK})

add_executable(tablor_clickprobe tools/clickprobe.cpp)
target_include_directories(tablor_clickprobe PRIVATE src src/host)
target_link_libraries(tablor_clickprobe PRIVATE dl m)
target_link_options(tablor_clickprobe PRIVATE ${TB_STATIC_LINK})

add_executable(tablor_wtformats tools/wtformats.cpp)
target_include_directories(tablor_wtformats PRIVATE src src/host)
target_link_libraries(tablor_wtformats PRIVATE dl m)
target_link_options(tablor_wtformats PRIVATE ${TB_STATIC_LINK})

add_executable(tablor_stucknote tools/stucknote.cpp)
target_include_directories(tablor_stucknote PRIVATE src src/host)
target_link_libraries(tablor_stucknote PRIVATE dl m)
target_link_options(tablor_stucknote PRIVATE ${TB_STATIC_LINK})

add_executable(tablor_wtprobe tools/wtprobe.cpp)
target_include_directories(tablor_wtprobe PRIVATE src src/host)
target_link_libraries(tablor_wtprobe PRIVATE dl m)
target_link_options(tablor_wtprobe PRIVATE ${TB_STATIC_LINK})
