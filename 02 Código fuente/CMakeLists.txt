cmake_minimum_required(VERSION 3.16)
project(electrical_tools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4)
pkg_check_modules(LIBADWAITA REQUIRED libadwaita-1)

find_path(NLOHMANN_INCLUDE_DIR nlohmann/json.hpp)
if(NOT NLOHMANN_INCLUDE_DIR)
    message(FATAL_ERROR "Se necesita nlohmann/json.hpp (paquete nlohmann-json3-dev).")
endif()

# Logo de la aplicación (res/logo.svg) incrustado en el ejecutable como cabecera generada
set(LOGO_SVG ${CMAKE_CURRENT_SOURCE_DIR}/res/logo.svg)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${LOGO_SVG})
file(READ ${LOGO_SVG} LOGO_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," LOGO_BYTES "${LOGO_HEX}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generado/logo_svg.h
     "// Generado por CMake a partir de res/logo.svg; no editar\n#pragma once\nstatic const unsigned char LOGO_SVG[] = {${LOGO_BYTES}0x00};\n")

# Iconos de la aplicación (res/iconos/*.svg) incrustados en el ejecutable: se instalan al arrancar como un
# tema de iconos propio, así el programa no depende del tema de iconos del escritorio
file(GLOB ICONOS_SVG CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/res/iconos/*.svg)
list(SORT ICONOS_SVG)
set(ICONOS_H "// Generado por CMake a partir de res/iconos/*.svg; no editar\n#pragma once\n")
string(APPEND ICONOS_H "struct IconoIncrustado { const char* nombre; const unsigned char* datos; };\n")
set(ICONOS_LISTA "")
set(ICONO_N 0)
foreach(ICONO ${ICONOS_SVG})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ICONO})
    get_filename_component(ICONO_NOMBRE ${ICONO} NAME_WE)
    file(READ ${ICONO} ICONO_HEX HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," ICONO_BYTES "${ICONO_HEX}")
    string(APPEND ICONOS_H "static const unsigned char ICONO_${ICONO_N}[] = {${ICONO_BYTES}0x00};\n")
    string(APPEND ICONOS_LISTA "    {\"${ICONO_NOMBRE}\", ICONO_${ICONO_N}},\n")
    math(EXPR ICONO_N "${ICONO_N} + 1")
endforeach()
string(APPEND ICONOS_H "static const IconoIncrustado ICONOS[] = {\n${ICONOS_LISTA}};\n")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generado/iconos.h "${ICONOS_H}")

add_executable(electrical-tools
    src/main.cpp
    src/model.cpp
    src/excel.cpp
    src/diagrama.cpp
)

target_include_directories(electrical-tools PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generado ${GTK4_INCLUDE_DIRS} ${LIBADWAITA_INCLUDE_DIRS} ${NLOHMANN_INCLUDE_DIR})
target_link_libraries(electrical-tools PRIVATE ${GTK4_LIBRARIES} ${LIBADWAITA_LIBRARIES})
target_compile_options(electrical-tools PRIVATE ${GTK4_CFLAGS_OTHER} ${LIBADWAITA_CFLAGS_OTHER} -Wall -Wextra)

# Instalación (p. ej. cmake --install build --prefix ~/.local): ejecutable, icono y lanzador
include(GNUInstallDirs)
install(TARGETS electrical-tools RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES res/logo.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME com.apespower.electricaltools.svg)
install(FILES res/com.apespower.electricaltools.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
