#pragma once

#include "UnrealVoxelSim/Events/Api/IPump.h"
#include "UnrealVoxelSim/Movement/Api/IUpdater.h"
#include "UnrealVoxelSim/Navigation/Api/ICommandProcessor.h"
#include "UnrealVoxelSim/Navigation/Api/IFollowingUpdater.h"
#include "UnrealVoxelSim/Navigation/Api/IPlanner.h"
#include "UnrealVoxelSim/Navigation/Voxel/Api/ITopologyUpdater.h"
#include "UnrealVoxelSim/Simulation/Api/ITickPipeline.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/ICommandProcessor.h"

namespace UnrealVoxelSim::Testbed::Qt
{

class SimulationPipeline final : public Simulation::Api::ITickPipeline
{
  public:
    SimulationPipeline(Voxel::Solid::Api::ICommandProcessor &solidCommands, Events::Api::IPump &events,
                       Navigation::Api::ICommandProcessor &navigationCommands,
                       Navigation::Voxel::Api::ITopologyUpdater &topology, Navigation::Api::IPlanner &planner,
                       Navigation::Api::IFollowingUpdater &following,
                       Movement::Api::IUpdater &movement) noexcept;

    void Step(Simulation::Api::StepContext context) override;

  private:
    Voxel::Solid::Api::ICommandProcessor &SolidCommands_;
    Events::Api::IPump &Events_;
    Navigation::Api::ICommandProcessor &NavigationCommands_;
    Navigation::Voxel::Api::ITopologyUpdater &Topology_;
    Navigation::Api::IPlanner &Planner_;
    Navigation::Api::IFollowingUpdater &Following_;
    Movement::Api::IUpdater &Movement_;
};

} // namespace UnrealVoxelSim::Testbed::Qt
