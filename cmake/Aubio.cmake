# aubio (GPLv3, https://aubio.org): we only use its pitch tracker (YIN-FFT). Built as a small
# static C library from the source files the pitch code needs; FFT = its bundled Ooura code.
FetchContent_Declare(aubio
    GIT_REPOSITORY https://github.com/aubio/aubio.git
    GIT_TAG        0.4.9
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  do-not-build)
FetchContent_MakeAvailable(aubio)

# aubio 0.4.9 bug: YIN-FFT reports confidence 0 for every pitch below ~1300 Hz (peak_pos is only set
# on the high-pitch path). Set it on both paths.
set(_yinfft "${aubio_SOURCE_DIR}/src/pitch/pitchyinfft.c")
file(READ "${_yinfft}" _src)
if(NOT _src MATCHES "fall patch")
    set(_anchor "    if (tau > p->short_period) {\n      output->data[0] = fvec_quadratic_peak_pos (yin, tau);")
    string(FIND "${_src}" "${_anchor}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "Aubio.cmake: pitchyinfft.c changed; update the confidence patch")
    endif()
    string(REPLACE "${_anchor}" "    if (tau > p->short_period) {\n      p->peak_pos = tau; /* fall patch: confidence */\n      output->data[0] = fvec_quadratic_peak_pos (yin, tau);" _src "${_src}")
    file(WRITE "${_yinfft}" "${_src}")
endif()

set(_a "${aubio_SOURCE_DIR}/src")
add_library(fall_aubio STATIC
    ${_a}/cvec.c ${_a}/fmat.c ${_a}/fvec.c ${_a}/lvec.c ${_a}/mathutils.c ${_a}/musicutils.c ${_a}/vecutils.c
    ${_a}/pitch/pitch.c ${_a}/pitch/pitchfcomb.c ${_a}/pitch/pitchmcomb.c ${_a}/pitch/pitchschmitt.c
    ${_a}/pitch/pitchspecacf.c ${_a}/pitch/pitchyin.c ${_a}/pitch/pitchyinfast.c ${_a}/pitch/pitchyinfft.c
    ${_a}/spectral/fft.c ${_a}/spectral/ooura_fft8g.c ${_a}/spectral/phasevoc.c
    ${_a}/temporal/filter.c ${_a}/temporal/c_weighting.c
    ${_a}/utils/log.c)
target_include_directories(fall_aubio PUBLIC ${_a})
target_compile_definitions(fall_aubio PRIVATE
    HAVE_STDLIB_H=1 HAVE_STDIO_H=1 HAVE_MATH_H=1 HAVE_STRING_H=1 HAVE_ERRNO_H=1 HAVE_LIMITS_H=1
    HAVE_STDARG_H=1 HAVE_C99_VARARGS_MACROS=1 _USE_MATH_DEFINES=1 _CRT_SECURE_NO_WARNINGS=1)
set_target_properties(fall_aubio PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(fall_aubio PRIVATE /W0)
else()
    target_compile_options(fall_aubio PRIVATE -w)
    target_link_libraries(fall_aubio PUBLIC m)
endif()
