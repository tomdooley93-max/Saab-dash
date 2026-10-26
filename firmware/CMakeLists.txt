cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.1.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(saab-dash)

# The built app (docs/) goes into the storage partition, so a freshly flashed dongle
# serves the app right away. Later app updates replace it via POST /api/app.
set(WWW ${CMAKE_BINARY_DIR}/www)
file(MAKE_DIRECTORY ${WWW})
file(COPY ${CMAKE_SOURCE_DIR}/../docs/index.html ${CMAKE_SOURCE_DIR}/../docs/manifest.json
          ${CMAKE_SOURCE_DIR}/../docs/icons DESTINATION ${WWW})
spiffs_create_partition_image(storage ${WWW} FLASH_IN_PROJECT)
