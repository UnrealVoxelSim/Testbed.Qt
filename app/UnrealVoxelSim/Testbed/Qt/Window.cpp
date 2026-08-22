#include "Window.h"

#include "Viewport.h"

#include "UnrealVoxelSim/Voxel/Solid/Api/StandardMaterials.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
#include <chrono>
#include <cstddef>

namespace UnrealVoxelSim::Testbed::Qt
{
namespace
{
constexpr Simulation::Api::TickCount MaximumTicksPerUiCallback{1};
}

Window::Window(UnrealVoxelSim::Simulation::Api::IPacer &pacer,
               UnrealVoxelSim::Simulation::Api::IStepper &stepper,
               const UnrealVoxelSim::Voxel::Api::IBounds &bounds,
               const UnrealVoxelSim::Voxel::Solid::Api::IReader &reader,
               const UnrealVoxelSim::Voxel::Solid::Api::IRegionReader &regionReader,
               UnrealVoxelSim::Voxel::Solid::Api::ICommandSink &commands,
               UnrealVoxelSim::Voxel::Solid::Api::IChangeSource &changes,
               UnrealVoxelSim::Navigation::Api::ICommandSink &navigationCommands,
               const UnrealVoxelSim::Navigation::Api::IExecutionReader &navigationExecutions,
               const UnrealVoxelSim::Movement::Api::IReader &movement,
               const UnrealVoxelSim::Ecs::Api::EntityId pawn, QWidget *parent)
    : QMainWindow(parent), Pacer_(pacer), Stepper_(stepper)
{
    setWindowTitle("UnrealVoxelSim Qt Testbed");

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    auto *toolbar = new QHBoxLayout;
    auto *mode = new QComboBox(central);
    auto *material = new QComboBox(central);
    auto *size = new QSpinBox(central);
    auto *renderDistance = new QSpinBox(central);
    auto *diagnostics = new QLabel(central);
    auto *simulationRate = new QComboBox(central);
    auto *singleStep = new QPushButton("Step", central);

    mode->addItem("Fill");
    mode->addItem("Erase");
    mode->addItem("Navigate");
    simulationRate->addItems({"Paused", "0.5x", "1x", "2x", "10x", "100x"});
    simulationRate->setCurrentIndex(2);
    material->addItem("Dirt", UnrealVoxelSim::Voxel::Solid::Api::StandardMaterials::Dirt.Value());
    material->addItem("Grass", UnrealVoxelSim::Voxel::Solid::Api::StandardMaterials::Grass.Value());
    material->addItem("Stone", UnrealVoxelSim::Voxel::Solid::Api::StandardMaterials::Stone.Value());
    size->setRange(1, 9);
    size->setValue(1);
    size->setSuffix(" cells");
    renderDistance->setRange(16, 512);
    renderDistance->setSingleStep(16);
    renderDistance->setValue(80);
    renderDistance->setSuffix(" cells");

    toolbar->addWidget(new QLabel("Brush:", central));
    toolbar->addWidget(mode);
    toolbar->addWidget(new QLabel("Solid:", central));
    toolbar->addWidget(material);
    toolbar->addWidget(new QLabel("Size:", central));
    toolbar->addWidget(size);
    toolbar->addWidget(new QLabel("Render:", central));
    toolbar->addWidget(renderDistance);
    toolbar->addWidget(new QLabel("Simulation:", central));
    toolbar->addWidget(simulationRate);
    toolbar->addWidget(singleStep);
    toolbar->addSpacing(12);
    toolbar->addWidget(diagnostics, 1);

    Viewport_ = new Viewport(bounds, reader, regionReader, commands, changes, Stepper_, navigationCommands,
                             navigationExecutions, movement, pawn, central);
    Viewport_->SetDiagnosticsSink([diagnostics](const QString &text) { diagnostics->setText(text); });
    layout->addLayout(toolbar);
    layout->addWidget(Viewport_, 1);
    setCentralWidget(central);

    connect(mode, &QComboBox::currentIndexChanged, this,
            [this](const int index) {
                Viewport_->SetBrushMode(index == 0 ? BrushMode::Fill : index == 1 ? BrushMode::Erase
                                                                                  : BrushMode::Navigate);
            });
    connect(material, &QComboBox::currentIndexChanged, this, [this, material](const int index) {
        Viewport_->SetMaterial(UnrealVoxelSim::Voxel::Solid::Api::MaterialId{material->itemData(index).toUInt()});
    });
    connect(size, &QSpinBox::valueChanged, this, [this](const int value) { Viewport_->SetBrushSize(value); });
    connect(renderDistance, &QSpinBox::valueChanged, this,
            [this](const int value) { Viewport_->SetRenderDistance(value); });
    constexpr std::array rates{UnrealVoxelSim::Simulation::Api::PausedRate,
                               *UnrealVoxelSim::Simulation::Api::Rate::Create(1, 2),
                               UnrealVoxelSim::Simulation::Api::NormalRate,
                               *UnrealVoxelSim::Simulation::Api::Rate::Create(2, 1),
                               *UnrealVoxelSim::Simulation::Api::Rate::Create(10, 1),
                               *UnrealVoxelSim::Simulation::Api::Rate::Create(100, 1)};
    connect(simulationRate, &QComboBox::currentIndexChanged, this,
            [this, rates](const int index) { Pacer_.SetRate(rates[static_cast<std::size_t>(index)]); });
    connect(singleStep, &QPushButton::clicked, this, [this] {
        if (!Stepper_.Step(UnrealVoxelSim::Simulation::Api::TickCount{1}))
            Viewport_->ReportStatus("Simulation tick overflow");
    });

    auto *timer = new QTimer(this);
    timer->setTimerType(::Qt::PreciseTimer);
    connect(timer, &QTimer::timeout, this, [this] {
        const auto elapsed = std::chrono::nanoseconds{SimulationClock_.nsecsElapsed()};
        SimulationClock_.restart();
        const auto result = Pacer_.Advance(elapsed, MaximumTicksPerUiCallback);
        if (!result)
            Viewport_->ReportStatus("Simulation pacing failed");
        else
            Viewport_->SetSimulationBacklog(result->Pending);
        Viewport_->Tick();
    });
    SimulationClock_.start();
    timer->start(0);
}

} // namespace UnrealVoxelSim::Testbed::Qt
