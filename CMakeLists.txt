cmake_minimum_required(VERSION 3.10 FATAL_ERROR)

project(cpu_tsdf)

find_package(PCL 1.7 QUIET REQUIRED)


# create the CPUTSDF library
add_library (cpu_tsdf SHARED
              src/lib/octree.cpp
              src/lib/tsdf_volume_octree.cpp
              src/lib/tsdf_interface.cpp
              src/lib/marching_cubes_tsdf_octree.cpp)

target_include_directories(cpu_tsdf
		PUBLIC
			include/
			${PCL_INCLUDE_DIRS}
			)
target_link_libraries (cpu_tsdf
		PUBLIC
			${PCL_LIBRARIES})

find_package(OpenMP)
if(OpenMP_CXX_FOUND)
	target_link_libraries(cpu_tsdf PUBLIC OpenMP::OpenMP_CXX)
endif()

add_library(cpu_tsdf::cpu_tsdf ALIAS cpu_tsdf)
