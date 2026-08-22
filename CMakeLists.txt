cmake_minimum_required(VERSION 3.30)

cmake_policy(SET CMP0168 NEW)

project(UnrealVoxelSim.Testbed.Qt VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)

set(UNREALVOXELSIM_BUILD_CMAKE_SOURCE "" CACHE PATH "Optional local source override for Build.CMake.")
if(UNREALVOXELSIM_BUILD_CMAKE_SOURCE)
    FetchContent_Declare(uvsbuild SOURCE_DIR "${UNREALVOXELSIM_BUILD_CMAKE_SOURCE}")
else()
    FetchContent_Declare(
        uvsbuild
        GIT_REPOSITORY https://github.com/UnrealVoxelSim/Build.CMake.git
        GIT_TAG main
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
    )
endif()
FetchContent_MakeAvailable(uvsbuild)

include("${uvsbuild_SOURCE_DIR}/UnrealVoxelSim.Build.CMake.cmake")

UnrealVoxelSimAddInternalDependency(GITHUB_REPOSITORY UnrealVoxelSim/Events.InMemory)
UnrealVoxelSimAddInternalDependency(GITHUB_REPOSITORY UnrealVoxelSim/Ecs.EnTT)
UnrealVoxelSimAddInternalDependency(GITHUB_REPOSITORY UnrealVoxelSim/Movement.Voxel)
UnrealVoxelSimAddInternalDependency(GITHUB_REPOSITORY UnrealVoxelSim/Navigation.Following)
UnrealVoxelSimAddInternalDependency(GITHUB_REPOSITORY UnrealVoxelSim/Navigation.Voxel)
UnrealVoxelSimAddInternalDependency(GITHUB_REPOSITORY UnrealVoxelSim/Simulation.FixedStep)
UnrealVoxelSimAddInternalDependency(GITHUB_REPOSITORY UnrealVoxelSim/Voxel.Chunked)
UnrealVoxelSimAddInternalDependency(GITHUB_REPOSITORY UnrealVoxelSim/Voxel.Solid)
UnrealVoxelSimAddInternalDependency(GITHUB_REPOSITORY UnrealVoxelSim/Voxel.Solid.Commands)
UnrealVoxelSimAddInternalDependency(GITHUB_REPOSITORY UnrealVoxelSim/Voxel.Solid.Navigation)
UnrealVoxelSimAddInternalDependency(GITHUB_REPOSITORY UnrealVoxelSim/Voxel.Solid.Rendering)

UnrealVoxelSimConfigureModule()
