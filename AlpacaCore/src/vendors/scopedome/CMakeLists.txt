cmake_minimum_required(VERSION 3.20)

# ScopeDome USB Card 2.1 domes (serial, no SDK).
set(SCOPEDOME_SOURCES
    scopedome_usb21_protocol_wrapper.cpp
    scopedome_dome_driver.cpp
    scopedome_catalog.cpp
)

add_library(alpacacore_scopedome STATIC ${SCOPEDOME_SOURCES})

target_include_directories(alpacacore_scopedome PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)
target_link_libraries(alpacacore_scopedome PRIVATE Threads::Threads)

# Links down to alpacacore, never the reverse (open-astro#710).
target_link_libraries(alpacacore_scopedome PRIVATE alpacacore)
