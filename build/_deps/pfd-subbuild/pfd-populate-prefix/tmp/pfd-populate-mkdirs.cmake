# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/home/zvp/Documents/GitHub/CVRTR/build/_deps/pfd-src")
  file(MAKE_DIRECTORY "/home/zvp/Documents/GitHub/CVRTR/build/_deps/pfd-src")
endif()
file(MAKE_DIRECTORY
  "/home/zvp/Documents/GitHub/CVRTR/build/_deps/pfd-build"
  "/home/zvp/Documents/GitHub/CVRTR/build/_deps/pfd-subbuild/pfd-populate-prefix"
  "/home/zvp/Documents/GitHub/CVRTR/build/_deps/pfd-subbuild/pfd-populate-prefix/tmp"
  "/home/zvp/Documents/GitHub/CVRTR/build/_deps/pfd-subbuild/pfd-populate-prefix/src/pfd-populate-stamp"
  "/home/zvp/Documents/GitHub/CVRTR/build/_deps/pfd-subbuild/pfd-populate-prefix/src"
  "/home/zvp/Documents/GitHub/CVRTR/build/_deps/pfd-subbuild/pfd-populate-prefix/src/pfd-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/zvp/Documents/GitHub/CVRTR/build/_deps/pfd-subbuild/pfd-populate-prefix/src/pfd-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/zvp/Documents/GitHub/CVRTR/build/_deps/pfd-subbuild/pfd-populate-prefix/src/pfd-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
