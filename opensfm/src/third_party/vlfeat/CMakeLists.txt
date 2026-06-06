file(GLOB VLFEAT_SRCS vl/*.c vl/*.h)

set(VLFEAT_X86 FALSE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i.86)$")
    set(VLFEAT_X86 TRUE)
endif()

if(OPENSFM_ENABLE_AVX AND VLFEAT_X86)
    if(MSVC)
        set_source_files_properties(vl/mathop_avx.c PROPERTIES COMPILE_FLAGS "/arch:AVX")
    else()
        set_source_files_properties(vl/mathop_avx.c PROPERTIES COMPILE_FLAGS "-mavx")
    endif()
else()
    add_definitions(-DVL_DISABLE_AVX)
endif()

if(WIN32)
    add_definitions(-D__SSE2__)
endif()

if( (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "aarch64") OR (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "arm64") )
    add_definitions( -DVL_DISABLE_SSE2 )
endif()

add_library(vl ${VLFEAT_SRCS})


target_include_directories(vl
  PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/vlfeat
    ${OpenMP_CXX_INCLUDE_DIR})
