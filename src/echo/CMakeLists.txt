cmake_minimum_required(VERSION 3.16)
project(ascolto_echo C)
if(NOT EXISTS "${SPEEX_SOURCE}/libspeexdsp/mdf.c")
    message(FATAL_ERROR "SPEEX_SOURCE must name the pinned SpeexDSP checkout")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/speexdsp_config_types.h"
    "#include <stdint.h>\ntypedef int16_t spx_int16_t;\ntypedef uint16_t spx_uint16_t;\ntypedef int32_t spx_int32_t;\ntypedef uint32_t spx_uint32_t;\n")
add_library(speexdsp STATIC
    "${SPEEX_SOURCE}/libspeexdsp/mdf.c"
    "${SPEEX_SOURCE}/libspeexdsp/preprocess.c"
    "${SPEEX_SOURCE}/libspeexdsp/filterbank.c"
    "${SPEEX_SOURCE}/libspeexdsp/fftwrap.c"
    "${SPEEX_SOURCE}/libspeexdsp/smallft.c")
target_include_directories(speexdsp PRIVATE "${SPEEX_SOURCE}/include" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(speexdsp PRIVATE FLOATING_POINT USE_SMALLFT EXPORT=)
set_target_properties(speexdsp PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
