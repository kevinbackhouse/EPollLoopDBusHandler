# Copyright 2021-2024 Kevin Backhouse.
#
# This file is part of EPollLoopDBusHandler.
#
# EPollLoopDBusHandler is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# EPollLoopDBusHandler is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with EPollLoopDBusHandler.  If not, see <https://www.gnu.org/licenses/>.

add_library(EPollLoopDBusHandler SHARED)

target_sources(
        EPollLoopDBusHandler PRIVATE
        EPollLoopDBusHandler.cpp
        ../../include/EPollLoopDBusHandler/EPollLoopDBusHandler.hpp)

target_include_directories(EPollLoopDBusHandler PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/EPollLoopDBusHandler>
        $<BUILD_INTERFACE:${EPollLoop_DIR}/include/EPollLoop>
        $<BUILD_INTERFACE:${DBusParse_DIR}/include/DBusParse>
        $<BUILD_INTERFACE:${DBusParse_DIR}/include/DBusParseUtils>
)

write_basic_package_version_file(EPollLoopDBusHandlerConfigVersion.cmake COMPATIBILITY ExactVersion)

install(TARGETS EPollLoopDBusHandler EXPORT EPollLoopDBusHandlerConfig
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(EXPORT EPollLoopDBusHandlerConfig DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/EPollLoopDBusHandler")

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/EPollLoopDBusHandlerConfigVersion.cmake
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/EPollLoopDBusHandler")
