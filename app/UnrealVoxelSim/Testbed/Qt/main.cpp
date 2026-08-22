#include "Window.h"
#include "SimulationPipeline.h"

#include "UnrealVoxelSim/Ecs/Api/RegistryScopeId.h"
#include "UnrealVoxelSim/Ecs/EnTT/Registry.h"
#include "UnrealVoxelSim/Events/InMemory/Dispatcher.h"
#include "UnrealVoxelSim/Movement/Api/GroundedProfile.h"
#include "UnrealVoxelSim/Movement/Voxel/Controller.h"
#include "UnrealVoxelSim/Navigation/Following/Controller.h"
#include "UnrealVoxelSim/Navigation/Voxel/Planner.h"
#include "UnrealVoxelSim/Simulation/FixedStep/Controller.h"
#include "UnrealVoxelSim/Voxel/Chunked/Field.h"
#include "UnrealVoxelSim/Voxel/Api/Region.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/Changed.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/Placement.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/StandardMaterials.h"
#include "UnrealVoxelSim/Voxel/Solid/Controller.h"
#include "UnrealVoxelSim/Voxel/Solid/Commands/Queue.h"
#include "UnrealVoxelSim/Voxel/Solid/Navigation/Environment.h"
#include "UnrealVoxelSim/Voxel/Solid/Navigation/InvalidationBridge.h"

#include <QApplication>
#include <QMessageBox>
#include <QSurfaceFormat>
#include <QTimer>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

int main(int argc, char *argv[])
{
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setSamples(4);
    format.setSwapInterval(0);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication application(argc, argv);

    UnrealVoxelSim::Events::InMemory::Dispatcher dispatcher;
    UnrealVoxelSim::Voxel::Chunked::Field field{{{-256, -256, -64}, {256, 256, 128}}};
    constexpr std::array materials{
        UnrealVoxelSim::Voxel::Solid::Api::StandardMaterials::Dirt,
        UnrealVoxelSim::Voxel::Solid::Api::StandardMaterials::Grass,
        UnrealVoxelSim::Voxel::Solid::Api::StandardMaterials::Stone,
    };
    UnrealVoxelSim::Voxel::Solid::Controller solids{
        field, field, field, materials, dispatcher.CreateChannel<UnrealVoxelSim::Voxel::Solid::Api::Changed>()};

    std::vector<UnrealVoxelSim::Voxel::Solid::Api::Placement> terrain;
    terrain.reserve(400'000);
    for (std::int32_t y = -96; y < 96; ++y)
    {
        for (std::int32_t x = -96; x < 96; ++x)
        {
            const auto height = static_cast<std::int32_t>(std::round(std::sin(static_cast<double>(x) * 0.055) * 3.0 +
                                                                     std::cos(static_cast<double>(y) * 0.047) * 3.0));
            for (std::int32_t z = -8; z <= height; ++z)
            {
                auto material = UnrealVoxelSim::Voxel::Solid::Api::StandardMaterials::Stone;
                if (z == height)
                {
                    material = UnrealVoxelSim::Voxel::Solid::Api::StandardMaterials::Grass;
                }
                else if (z >= height - 2)
                {
                    material = UnrealVoxelSim::Voxel::Solid::Api::StandardMaterials::Dirt;
                }
                terrain.push_back({{x, y, z}, material});
            }
        }
    }
    if (!solids.Place(terrain))
    {
        QMessageBox::critical(nullptr, "Testbed initialization", "The initial solid terrain could not be created.");
        return 1;
    }

    constexpr std::array movementProfiles{
        UnrealVoxelSim::Movement::Api::GroundedProfile{UnrealVoxelSim::Movement::Api::ProfileId{1}}};
    UnrealVoxelSim::Ecs::EnTT::Registry entities{UnrealVoxelSim::Ecs::Api::RegistryScopeId{1}};
    const auto pawn = entities.Create();
    UnrealVoxelSim::Movement::Voxel::Controller movement{solids, movementProfiles};
    constexpr auto half = UnrealVoxelSim::Movement::Api::Scalar::OneRaw / 2;
    const auto registered = movement.Add(
        {pawn, movementProfiles[0].Id,
         {UnrealVoxelSim::Movement::Api::Scalar::FromRaw(half),
          UnrealVoxelSim::Movement::Api::Scalar::FromRaw(half),
          UnrealVoxelSim::Movement::Api::Scalar::FromWhole(4)}});
    if (!registered)
    {
        QMessageBox::critical(nullptr, "Testbed initialization", "The navigation pawn could not be spawned.");
        return 1;
    }

    UnrealVoxelSim::Voxel::Solid::Navigation::Environment navigationEnvironment{field, solids};
    UnrealVoxelSim::Voxel::Solid::Commands::Queue solidCommands{solids};
    constexpr std::size_t fineExpansionsPerTick = 64;
    constexpr std::size_t componentExpansionsPerTick = 4;
    UnrealVoxelSim::Navigation::Voxel::Planner planner{
        navigationEnvironment, movementProfiles, fineExpansionsPerTick,
        UnrealVoxelSim::Navigation::Voxel::Planner::DefaultMaximumExpansionsPerRequest,
        componentExpansionsPerTick,
        UnrealVoxelSim::Navigation::Voxel::Planner::DefaultTileBuildsPerTopologyUpdate};
    constexpr std::array initialNavigationRegions{
        UnrealVoxelSim::Voxel::Api::Region{{-96, -96, -8}, {96, 96, 7}}};
    planner.Prepare(initialNavigationRegions);
    UnrealVoxelSim::Navigation::Following::Controller following{planner, movement, movement, movementProfiles};
    UnrealVoxelSim::Voxel::Solid::Navigation::InvalidationBridge navigationInvalidation{solids.Changes(), planner};
    UnrealVoxelSim::Testbed::Qt::SimulationPipeline pipeline{solidCommands, dispatcher, following, planner, planner,
                                                             following, movement};
    UnrealVoxelSim::Simulation::FixedStep::Controller simulation{pipeline};

    UnrealVoxelSim::Testbed::Qt::Window window{simulation, simulation, field, solids, solids, solidCommands, solids.Changes(),
                                               following, following, movement, pawn};
    window.resize(1280, 800);
    window.show();

    if (application.arguments().contains("--smoke-test"))
    {
        QTimer::singleShot(1200, &application, &QApplication::quit);
    }
    return application.exec();
}
