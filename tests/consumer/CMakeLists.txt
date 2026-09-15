cmake_minimum_required(VERSION 3.25)
project(FabricCapabilityRegistryConsumer LANGUAGES CXX)

# Independent downstream consumer: only the installed package is visible.
find_package(FabricCapabilityRegistry CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE SummonSoftwareLabs::FabricCapabilityRegistry)
set_target_properties(consumer PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

if(MSVC)
  target_compile_options(consumer PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
endif()
