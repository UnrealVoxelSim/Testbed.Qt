add_library(
    UnrealVoxelSim.Testbed.Worlds
    UnrealVoxelSim/Testbed/PawnController.cpp
    UnrealVoxelSim/Testbed/PawnController.h
    UnrealVoxelSim/Testbed/VoxelMaterials.cpp
    UnrealVoxelSim/Testbed/VoxelMaterials.h
    UnrealVoxelSim/Testbed/World.cpp
)

target_include_directories(
    UnrealVoxelSim.Testbed.Worlds
    PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
)

target_link_libraries(
    UnrealVoxelSim.Testbed.Worlds
    PUBLIC
        UnrealVoxelSim.Ecs.Api
        UnrealVoxelSim.Movement.Api
        UnrealVoxelSim.Navigation.Api
        UnrealVoxelSim.Profiling.Api
        UnrealVoxelSim.Simulation.Api
        UnrealVoxelSim.Voxel.Api
        UnrealVoxelSim.Voxel.Rendering.Api
        UnrealVoxelSim.Voxel.Solid.Api
    PRIVATE
        UnrealVoxelSim.Composition.Game
        UnrealVoxelSim.Ecs.EnTT
)

if(UNREALVOXELSIM_ENABLE_PROFILING)
    target_compile_definitions(UnrealVoxelSim.Testbed.Worlds PRIVATE UNREALVOXELSIM_PROFILING_ENABLED=1)
endif()

UnrealVoxelSimConfigureTarget(UnrealVoxelSim.Testbed.Worlds)
