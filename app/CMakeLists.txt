find_package(Qt6 REQUIRED COMPONENTS OpenGL OpenGLWidgets Widgets)

qt_add_executable(
    ${PROJECT_NAME}.App
    UnrealVoxelSim/Testbed/Qt/BrushMode.h
    UnrealVoxelSim/Testbed/Qt/SimulationPipeline.cpp
    UnrealVoxelSim/Testbed/Qt/SimulationPipeline.h
    UnrealVoxelSim/Testbed/Qt/Viewport.cpp
    UnrealVoxelSim/Testbed/Qt/Viewport.h
    UnrealVoxelSim/Testbed/Qt/Window.cpp
    UnrealVoxelSim/Testbed/Qt/Window.h
    UnrealVoxelSim/Testbed/Qt/main.cpp
)

target_link_libraries(
    ${PROJECT_NAME}.App
    PRIVATE
        UnrealVoxelSim.Events.Api
        UnrealVoxelSim.Events.InMemory
        UnrealVoxelSim.Ecs.EnTT
        UnrealVoxelSim.Movement.Api
        UnrealVoxelSim.Movement.Voxel
        UnrealVoxelSim.Navigation.Api
        UnrealVoxelSim.Navigation.Following
        UnrealVoxelSim.Navigation.Voxel
        UnrealVoxelSim.Navigation.Voxel.Api
        UnrealVoxelSim.Simulation.Api
        UnrealVoxelSim.Simulation.FixedStep
        UnrealVoxelSim.Voxel.Api
        UnrealVoxelSim.Voxel.Chunked
        UnrealVoxelSim.Voxel.Rendering.Api
        UnrealVoxelSim.Voxel.Solid
        UnrealVoxelSim.Voxel.Solid.Api
        UnrealVoxelSim.Voxel.Solid.Commands
        UnrealVoxelSim.Voxel.Solid.Navigation
        UnrealVoxelSim.Voxel.Solid.Rendering
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        Qt6::Widgets
)

UnrealVoxelSimConfigureTarget(${PROJECT_NAME}.App)

if(WIN32)
    qt_import_plugins(${PROJECT_NAME}.App INCLUDE Qt6::QWindowsIntegrationPlugin)
endif()
