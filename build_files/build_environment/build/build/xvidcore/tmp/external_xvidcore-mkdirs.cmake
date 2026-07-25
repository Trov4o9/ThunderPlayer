# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "E:/Mundo Aberto Game Dev/ThunderPLayer/ThunderPLayer/ThunderPLayer/upbge-0.2.5b/build_files/build_environment/build/build/xvidcore/src/external_xvidcore")
  file(MAKE_DIRECTORY "E:/Mundo Aberto Game Dev/ThunderPLayer/ThunderPLayer/ThunderPLayer/upbge-0.2.5b/build_files/build_environment/build/build/xvidcore/src/external_xvidcore")
endif()
file(MAKE_DIRECTORY
  "E:/Mundo Aberto Game Dev/ThunderPLayer/ThunderPLayer/ThunderPLayer/upbge-0.2.5b/build_files/build_environment/build/build/xvidcore/src/external_xvidcore-build"
  "E:/Mundo Aberto Game Dev/ThunderPLayer/ThunderPLayer/ThunderPLayer/upbge-0.2.5b/build_files/build_environment/build/Release/xvidcore"
  "E:/Mundo Aberto Game Dev/ThunderPLayer/ThunderPLayer/ThunderPLayer/upbge-0.2.5b/build_files/build_environment/build/build/xvidcore/tmp"
  "E:/Mundo Aberto Game Dev/ThunderPLayer/ThunderPLayer/ThunderPLayer/upbge-0.2.5b/build_files/build_environment/build/build/xvidcore/src/external_xvidcore-stamp"
  "E:/Mundo Aberto Game Dev/ThunderPLayer/ThunderPLayer/ThunderPLayer/upbge-0.2.5b/build_files/build_environment/build/build/xvidcore/src"
  "E:/Mundo Aberto Game Dev/ThunderPLayer/ThunderPLayer/ThunderPLayer/upbge-0.2.5b/build_files/build_environment/build/build/xvidcore/src/external_xvidcore-stamp"
)

set(configSubDirs Debug;Release;MinSizeRel;RelWithDebInfo)
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "E:/Mundo Aberto Game Dev/ThunderPLayer/ThunderPLayer/ThunderPLayer/upbge-0.2.5b/build_files/build_environment/build/build/xvidcore/src/external_xvidcore-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "E:/Mundo Aberto Game Dev/ThunderPLayer/ThunderPLayer/ThunderPLayer/upbge-0.2.5b/build_files/build_environment/build/build/xvidcore/src/external_xvidcore-stamp${cfgdir}") # cfgdir has leading slash
endif()
