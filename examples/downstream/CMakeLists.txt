cmake_minimum_required(VERSION 3.25)

# An independent consumer of the installed package.
#
# This project is deliberately not part of the Loss Observatory build tree: it
# discovers the library only through find_package, exactly as a downstream
# project would. Building and running it proves the export set, the include
# interface, and the version file are usable from outside the source tree.

project(LossObservatoryDownstream CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(LossObservatory 1.0 REQUIRED)

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE LossObservatory::loss_observatory)

if(MSVC)
  target_compile_options(downstream_consumer PRIVATE /W4 /WX /permissive-)
endif()
