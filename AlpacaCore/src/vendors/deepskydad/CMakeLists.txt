cmake_minimum_required(VERSION 3.20)

# Deep Sky Dad FP1/FP2 flat panel: a plain serial protocol over the panel's USB
# CDC port (115200 8N1), from INDI's deepskydad_fp driver. No vendor SDK.
set(DEEPSKYDAD_SOURCES
    deepskydad_flatpanel_protocol_wrapper.cpp
    deepskydad_covercalibrator_driver.cpp
    deepskydad_catalog.cpp
)

add_library(alpacacore_deepskydad STATIC ${DEEPSKYDAD_SOURCES})

target_include_directories(alpacacore_deepskydad PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)
target_link_libraries(alpacacore_deepskydad PRIVATE Threads::Threads)

# Links down to alpacacore, never the reverse (open-astro#710).
target_link_libraries(alpacacore_deepskydad PRIVATE alpacacore)
