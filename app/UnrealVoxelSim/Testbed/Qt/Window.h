#pragma once

#include "UnrealVoxelSim/Ecs/Api/EntityId.h"
#include "UnrealVoxelSim/Movement/Api/IReader.h"
#include "UnrealVoxelSim/Navigation/Api/ICommandSink.h"
#include "UnrealVoxelSim/Navigation/Api/IExecutionReader.h"
#include "UnrealVoxelSim/Simulation/Api/IPacer.h"
#include "UnrealVoxelSim/Simulation/Api/IStepper.h"
#include "UnrealVoxelSim/Voxel/Api/IBounds.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/IChangeSource.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/ICommandSink.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/IReader.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/IRegionReader.h"

#include <QMainWindow>
#include <QElapsedTimer>

namespace UnrealVoxelSim::Testbed::Qt
{

class Viewport;

class Window final : public QMainWindow
{
  public:
    Window(UnrealVoxelSim::Simulation::Api::IPacer &pacer, UnrealVoxelSim::Simulation::Api::IStepper &stepper,
           const UnrealVoxelSim::Voxel::Api::IBounds &bounds,
           const UnrealVoxelSim::Voxel::Solid::Api::IReader &reader,
           const UnrealVoxelSim::Voxel::Solid::Api::IRegionReader &regionReader,
           UnrealVoxelSim::Voxel::Solid::Api::ICommandSink &commands,
           UnrealVoxelSim::Voxel::Solid::Api::IChangeSource &changes,
           UnrealVoxelSim::Navigation::Api::ICommandSink &navigationCommands,
           const UnrealVoxelSim::Navigation::Api::IExecutionReader &navigationExecutions,
           const UnrealVoxelSim::Movement::Api::IReader &movement, UnrealVoxelSim::Ecs::Api::EntityId pawn,
           QWidget *parent = nullptr);

  private:
    UnrealVoxelSim::Simulation::Api::IPacer &Pacer_;
    UnrealVoxelSim::Simulation::Api::IStepper &Stepper_;
    QElapsedTimer SimulationClock_;
    Viewport *Viewport_{};
};

} // namespace UnrealVoxelSim::Testbed::Qt
