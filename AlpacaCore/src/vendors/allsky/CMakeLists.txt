cmake_minimum_required(VERSION 3.20)

# Allsky camera cloud cover and the weather safety monitor (libcurl; JPEGs decoded by the vendored
# stb_image, compiled static into allsky_image_analysis.cpp; the
# safety monitor asks OpenWeather through alpacacore_openweather).
find_package(CURL REQUIRED)

set(ALLSKY_SOURCES
    allsky_image_analysis.cpp
    allsky_drivers.cpp
    allsky_catalog.cpp
)

add_library(alpacacore_allsky STATIC ${ALLSKY_SOURCES})

target_include_directories(alpacacore_allsky PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../external/stb
)

find_package(Threads REQUIRED)
target_link_libraries(alpacacore_allsky PRIVATE Threads::Threads CURL::libcurl alpacacore_openweather)

# Links down to alpacacore, never the reverse (open-astro#710).
target_link_libraries(alpacacore_allsky PRIVATE alpacacore)
