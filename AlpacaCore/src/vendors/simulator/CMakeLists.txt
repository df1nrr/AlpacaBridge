cmake_minimum_required(VERSION 3.20)

# Simulated devices: no hardware, no SDK. A dome today.
set(SIMULATOR_SOURCES
    simulator_dome_model.cpp
    simulator_dome_driver.cpp
    simulator_catalog.cpp
)

add_library(alpacacore_simulator STATIC ${SIMULATOR_SOURCES})

target_include_directories(alpacacore_simulator PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)
target_link_libraries(alpacacore_simulator PRIVATE Threads::Threads)

# Links down to alpacacore, never the reverse (open-astro#710).
target_link_libraries(alpacacore_simulator PRIVATE alpacacore)
