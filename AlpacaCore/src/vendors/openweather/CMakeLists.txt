cmake_minimum_required(VERSION 3.20)

# OpenWeather current weather (HTTPS through libcurl, no SDK).
find_package(CURL REQUIRED)

set(OPENWEATHER_SOURCES
    openweather_observingconditions_driver.cpp
    openweather_catalog.cpp
)

add_library(alpacacore_openweather STATIC ${OPENWEATHER_SOURCES})

target_include_directories(alpacacore_openweather PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)
target_link_libraries(alpacacore_openweather PRIVATE Threads::Threads CURL::libcurl)

# Links down to alpacacore, never the reverse (open-astro#710).
target_link_libraries(alpacacore_openweather PRIVATE alpacacore)
