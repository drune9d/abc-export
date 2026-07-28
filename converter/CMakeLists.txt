cmake_minimum_required(VERSION 3.18)

project(mesh2abc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${REPO_ROOT}/bin")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config} "${REPO_ROOT}/bin")
endforeach()

add_executable(mesh2abc
  mesh2abc.cpp
)

if(MSVC)
  target_compile_options(mesh2abc PRIVATE /W4 /wd4018 /wd4267 /wd4996)
else()
  target_compile_options(mesh2abc PRIVATE
    -Wall
    -Wextra
    -Wno-sign-compare
  )
endif()

if(APPLE)
  set(HOMEBREW_PREFIXES)
  if(DEFINED ENV{HOMEBREW_PREFIX} AND EXISTS "$ENV{HOMEBREW_PREFIX}")
    list(APPEND HOMEBREW_PREFIXES "$ENV{HOMEBREW_PREFIX}")
  endif()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    list(APPEND HOMEBREW_PREFIXES "/opt/homebrew" "/usr/local")
  else()
    list(APPEND HOMEBREW_PREFIXES "/usr/local" "/opt/homebrew")
  endif()

  set(HOMEBREW_PREFIX "")
  foreach(prefix IN LISTS HOMEBREW_PREFIXES)
    if(EXISTS "${prefix}")
      set(HOMEBREW_PREFIX "${prefix}")
      break()
    endif()
  endforeach()

  if(NOT HOMEBREW_PREFIX)
    message(FATAL_ERROR "Homebrew was not found. Install Homebrew, then run: brew install alembic")
  endif()

  message(STATUS "Using Homebrew prefix: ${HOMEBREW_PREFIX}")

  list(APPEND CMAKE_PREFIX_PATH
    "${HOMEBREW_PREFIX}"
    "${HOMEBREW_PREFIX}/opt/alembic"
    "${HOMEBREW_PREFIX}/opt/hdf5"
    "${HOMEBREW_PREFIX}/opt/imath"
    "${HOMEBREW_PREFIX}/opt/ilmbase"
    "${HOMEBREW_PREFIX}/opt/zlib"
  )

  find_package(Alembic CONFIG QUIET)
  find_package(HDF5 COMPONENTS C HL QUIET)
  find_package(Imath CONFIG QUIET)
  find_path(ALEMBIC_INCLUDE_DIR
    NAMES Alembic/AbcGeom/All.h
    PATHS "${HOMEBREW_PREFIX}/opt/alembic/include" "${HOMEBREW_PREFIX}/include"
    NO_DEFAULT_PATH
  )
  find_library(ALEMBIC_LIBRARY
    NAMES Alembic alembic
    PATHS "${HOMEBREW_PREFIX}/opt/alembic/lib" "${HOMEBREW_PREFIX}/lib"
    NO_DEFAULT_PATH
  )

  find_path(IMATH_INCLUDE_DIR
    NAMES Imath/ImathVec.h OpenEXR/ImathVec.h ImathVec.h
    PATHS
      "${HOMEBREW_PREFIX}/opt/imath/include"
      "${HOMEBREW_PREFIX}/opt/imath/include/Imath"
      "${HOMEBREW_PREFIX}/opt/ilmbase/include"
      "${HOMEBREW_PREFIX}/include"
    NO_DEFAULT_PATH
  )
  find_library(IMATH_LIBRARY
    NAMES Imath Half IlmImf IlmThread Iex
    PATHS "${HOMEBREW_PREFIX}/opt/imath/lib" "${HOMEBREW_PREFIX}/opt/ilmbase/lib" "${HOMEBREW_PREFIX}/lib"
    NO_DEFAULT_PATH
  )

  find_path(HOMEBREW_ZLIB_INCLUDE_DIR
    NAMES zlib.h
    PATHS "${HOMEBREW_PREFIX}/opt/zlib/include" "${HOMEBREW_PREFIX}/include"
    NO_DEFAULT_PATH
  )
  find_library(HOMEBREW_ZLIB_LIBRARY
    NAMES z
    PATHS "${HOMEBREW_PREFIX}/opt/zlib/lib" "${HOMEBREW_PREFIX}/lib"
    NO_DEFAULT_PATH
  )

  if(NOT Alembic_FOUND AND (NOT ALEMBIC_INCLUDE_DIR OR NOT ALEMBIC_LIBRARY))
    message(FATAL_ERROR "Alembic was not found in Homebrew. Run: brew install alembic")
  endif()

  if(NOT HDF5_FOUND)
    message(FATAL_ERROR "HDF5 was not found in Homebrew. Run: brew install hdf5")
  endif()

  if(NOT Imath_FOUND AND (NOT IMATH_INCLUDE_DIR OR NOT IMATH_LIBRARY))
    message(FATAL_ERROR "Imath/IlmBase was not found in Homebrew. Run: brew install imath")
  endif()

  if(NOT HOMEBREW_ZLIB_INCLUDE_DIR OR NOT HOMEBREW_ZLIB_LIBRARY)
    message(FATAL_ERROR "zlib was not found in Homebrew. Run: brew install zlib")
  endif()

  add_library(Homebrew::ZLIB UNKNOWN IMPORTED)
  set_target_properties(Homebrew::ZLIB PROPERTIES
    IMPORTED_LOCATION "${HOMEBREW_ZLIB_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${HOMEBREW_ZLIB_INCLUDE_DIR}"
  )

  if(Alembic_FOUND AND TARGET Alembic::Alembic)
    target_link_libraries(mesh2abc PRIVATE Alembic::Alembic)
  elseif(Alembic_FOUND AND TARGET Alembic::AlembicShared)
    target_link_libraries(mesh2abc PRIVATE Alembic::AlembicShared)
  else()
    target_include_directories(mesh2abc PRIVATE "${ALEMBIC_INCLUDE_DIR}")
    target_link_libraries(mesh2abc PRIVATE "${ALEMBIC_LIBRARY}")
  endif()

  if(Imath_FOUND)
    if(TARGET Imath::Imath)
      target_link_libraries(mesh2abc PRIVATE Imath::Imath)
    elseif(TARGET Imath::Half)
      target_link_libraries(mesh2abc PRIVATE Imath::Half)
    endif()
  else()
    target_include_directories(mesh2abc PRIVATE "${IMATH_INCLUDE_DIR}")
    target_link_libraries(mesh2abc PRIVATE "${IMATH_LIBRARY}")
  endif()

  if(TARGET HDF5::HDF5)
    target_link_libraries(mesh2abc PRIVATE HDF5::HDF5)
  else()
    target_include_directories(mesh2abc PRIVATE ${HDF5_INCLUDE_DIRS})
    target_link_libraries(mesh2abc PRIVATE ${HDF5_LIBRARIES} ${HDF5_HL_LIBRARIES})
  endif()

  target_link_libraries(mesh2abc PRIVATE Homebrew::ZLIB)

  if(ALEMBIC_INCLUDE_DIR)
    target_include_directories(mesh2abc PRIVATE "${ALEMBIC_INCLUDE_DIR}")
  endif()
  if(IMATH_INCLUDE_DIR)
    target_include_directories(mesh2abc PRIVATE "${IMATH_INCLUDE_DIR}")
  endif()
elseif(WIN32)
  set(HOUDINI_ROOT "" CACHE PATH "Houdini install directory")
  if(NOT HOUDINI_ROOT AND DEFINED ENV{HFS})
    set(HOUDINI_ROOT "$ENV{HFS}")
  endif()
  if(NOT HOUDINI_ROOT AND DEFINED ENV{HOUDINI_ROOT})
    set(HOUDINI_ROOT "$ENV{HOUDINI_ROOT}")
  endif()

  if(NOT HOUDINI_ROOT)
    message(FATAL_ERROR "Houdini was not found. Pass -DHOUDINI_ROOT=\"C:/Program Files/Side Effects Software/Houdini <version>\".")
  endif()

  file(TO_CMAKE_PATH "${HOUDINI_ROOT}" HOUDINI_ROOT)
  message(STATUS "Using Houdini root: ${HOUDINI_ROOT}")

  find_path(HOUDINI_ALEMBIC_INCLUDE_DIR
    NAMES Alembic/AbcGeom/All.h
    PATHS "${HOUDINI_ROOT}/toolkit/include"
    NO_DEFAULT_PATH
  )
  find_path(HOUDINI_HALF_INCLUDE_DIR
    NAMES half.h
    PATHS
      "${HOUDINI_ROOT}/toolkit/include/Imath"
      "${HOUDINI_ROOT}/toolkit/include/OpenEXR"
    NO_DEFAULT_PATH
  )
  find_library(HOUDINI_ALEMBIC_LIBRARY
    NAMES Alembic_sidefx
    PATHS "${HOUDINI_ROOT}/custom/houdini/dsolib"
    NO_DEFAULT_PATH
  )
  find_library(HOUDINI_IMATH_LIBRARY
    NAMES Imath_sidefx
    PATHS "${HOUDINI_ROOT}/custom/houdini/dsolib"
    NO_DEFAULT_PATH
  )
  find_library(HOUDINI_IEX_LIBRARY
    NAMES Iex_sidefx
    PATHS "${HOUDINI_ROOT}/custom/houdini/dsolib"
    NO_DEFAULT_PATH
  )
  find_library(HOUDINI_ILMTHREAD_LIBRARY
    NAMES IlmThread_sidefx
    PATHS "${HOUDINI_ROOT}/custom/houdini/dsolib"
    NO_DEFAULT_PATH
  )

  if(NOT HOUDINI_ALEMBIC_INCLUDE_DIR OR NOT HOUDINI_ALEMBIC_LIBRARY)
    message(FATAL_ERROR "Houdini Alembic headers/libraries were not found under: ${HOUDINI_ROOT}")
  endif()

  target_include_directories(mesh2abc PRIVATE
    "${HOUDINI_ALEMBIC_INCLUDE_DIR}"
    "${HOUDINI_HALF_INCLUDE_DIR}"
  )
  target_link_libraries(mesh2abc PRIVATE
    "${HOUDINI_ALEMBIC_LIBRARY}"
    "${HOUDINI_IMATH_LIBRARY}"
    "${HOUDINI_IEX_LIBRARY}"
    "${HOUDINI_ILMTHREAD_LIBRARY}"
  )
else()
  message(FATAL_ERROR "This CMake build currently targets macOS and Windows only.")
endif()

set_target_properties(mesh2abc PROPERTIES
  OUTPUT_NAME "mesh2abc"
  RUNTIME_OUTPUT_DIRECTORY "${REPO_ROOT}/bin"
)
