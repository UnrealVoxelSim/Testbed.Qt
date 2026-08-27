find_package(Qt6 REQUIRED COMPONENTS OpenGL OpenGLWidgets Widgets)

qt_add_executable(
    ${PROJECT_NAME}.App
    UnrealVoxelSim/Testbed/Qt/Tool.h
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
        UnrealVoxelSim.Profiling.Api
        UnrealVoxelSim.Testbed.Worlds
        UnrealVoxelSim.Voxel.Rendering.Api
        UnrealVoxelSim.Voxel.Solid.Api
        UnrealVoxelSim.Voxel.Solid.Rendering
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        Qt6::Widgets
)

if(UNREALVOXELSIM_ENABLE_PROFILING)
    target_link_libraries(${PROJECT_NAME}.App PRIVATE UnrealVoxelSim.Profiling.Tracy)
    target_compile_definitions(${PROJECT_NAME}.App PRIVATE UNREALVOXELSIM_PROFILING_ENABLED=1)
endif()

UnrealVoxelSimConfigureTarget(${PROJECT_NAME}.App)

if(WIN32)
    qt_import_plugins(${PROJECT_NAME}.App INCLUDE Qt6::QWindowsIntegrationPlugin)
endif()

add_executable(
    ${PROJECT_NAME}.Headless
    UnrealVoxelSim/Testbed/Qt/Headless.cpp
)

target_link_libraries(
    ${PROJECT_NAME}.Headless
    PRIVATE
        UnrealVoxelSim.Profiling.Api
        UnrealVoxelSim.Simulation.Api
        UnrealVoxelSim.Testbed.Worlds
)

if(UNREALVOXELSIM_ENABLE_PROFILING)
    target_link_libraries(${PROJECT_NAME}.Headless PRIVATE UnrealVoxelSim.Profiling.Tracy)
    target_compile_definitions(${PROJECT_NAME}.Headless PRIVATE UNREALVOXELSIM_PROFILING_ENABLED=1)
endif()

UnrealVoxelSimConfigureTarget(${PROJECT_NAME}.Headless)
