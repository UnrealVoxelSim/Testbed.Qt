#include "SimulationPipeline.h"

#include <stdexcept>

namespace UnrealVoxelSim::Testbed::Qt
{

SimulationPipeline::SimulationPipeline(Voxel::Solid::Api::ICommandProcessor &solidCommands, Events::Api::IPump &events,
                                       Navigation::Api::ICommandProcessor &navigationCommands,
                                       Navigation::Voxel::Api::ITopologyUpdater &topology,
                                       Navigation::Api::IPlanner &planner, Navigation::Api::IFollowingUpdater &following,
                                       Movement::Api::IUpdater &movement) noexcept
    : SolidCommands_(solidCommands), Events_(events), NavigationCommands_(navigationCommands), Topology_(topology),
      Planner_(planner), Following_(following), Movement_(movement)
{
}

void SimulationPipeline::Step(const Simulation::Api::StepContext context)
{
    // This explicit order belongs to the Testbed composition root, not the fixed-step engine.
    SolidCommands_.ProcessCommands(context);
    if (!Events_.DispatchPending())
    {
        throw std::logic_error{"Pre-navigation event dispatch was rejected."};
    }
    NavigationCommands_.ProcessCommands(context);
    Topology_.UpdateTopology(context);
    Planner_.Advance(context);
    Following_.UpdateFollowing(context);
    Movement_.Update(context);
    if (!Events_.DispatchPending())
    {
        throw std::logic_error{"Post-movement event dispatch was rejected."};
    }
}

} // namespace UnrealVoxelSim::Testbed::Qt
