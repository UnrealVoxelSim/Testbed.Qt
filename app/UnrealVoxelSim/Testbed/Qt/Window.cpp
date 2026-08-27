#include "Window.h"

#include "Tool.h"
#include "Viewport.h"

#include "UnrealVoxelSim/Profiling/Api/Macros.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/StandardMaterials.h"

#include <QAction>
#include <QActionGroup>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>

namespace UnrealVoxelSim::Testbed::Qt
{
namespace
{
constexpr Simulation::Api::TickCount MaximumTicksPerUiCallback{1};
constexpr std::array Rates{Simulation::Api::PausedRate,
                           *Simulation::Api::Rate::Create(1, 2),
                           Simulation::Api::NormalRate,
                           *Simulation::Api::Rate::Create(2, 1),
                           *Simulation::Api::Rate::Create(10, 1),
                           *Simulation::Api::Rate::Create(100, 1)};
}

Window::Window(const std::string_view initialWorld, Profiling::Api::IRecorder &profiling, QWidget *parent)
    : QMainWindow(parent), m_Profiling(profiling)
{
    setWindowTitle("UnrealVoxelSim Qt Testbed");

    auto *fileMenu = menuBar()->addMenu("&File");
    auto *worldMenu = fileMenu->addMenu("World");
    auto *worldActions = new QActionGroup(this);
    worldActions->setExclusive(true);
    for (const auto &descriptor : WorldCatalog::Worlds())
    {
        auto *action = worldMenu->addAction(QString::fromUtf8(descriptor.DisplayName.data(),
                                                              static_cast<qsizetype>(descriptor.DisplayName.size())));
        action->setCheckable(true);
        action->setData(QString::fromUtf8(descriptor.Id.data(), static_cast<qsizetype>(descriptor.Id.size())));
        worldActions->addAction(action);
        connect(action, &QAction::triggered, this, [this, action] {
            LoadWorld(action->data().toString().toStdString());
        });
    }
    fileMenu->addSeparator();
    fileMenu->addAction("E&xit", this, &QWidget::close);

    LoadWorld(initialWorld);
    if (!m_World) throw std::runtime_error{"The initial testbed world could not be created."};

    auto *timer = new QTimer(this);
    timer->setTimerType(::Qt::PreciseTimer);
    connect(timer, &QTimer::timeout, this, [this] {
        if (!m_World || !m_Viewport) return;
        UNREALVOXELSIM_PROFILE_ZONE(m_Profiling, "Qt update callback");
        const auto elapsed = std::chrono::nanoseconds{m_SimulationClock.nsecsElapsed()};
        m_SimulationClock.restart();
        const auto result = m_World->Pacer().Advance(elapsed, MaximumTicksPerUiCallback);
        if (!result)
            m_Viewport->ReportStatus("Simulation pacing failed");
        else
            m_Viewport->SetSimulationBacklog(result->Pending);
        if (result)
            UNREALVOXELSIM_PROFILE_PLOT(m_Profiling, "Simulation backlog", result->Pending.Value());
        m_Viewport->Tick();
    });
    m_SimulationClock.start();
    timer->start(0);
}

World &Window::CurrentWorld() noexcept
{
    return *m_World;
}

void Window::LoadWorld(const std::string_view id)
{
    if (m_World && m_World->Descriptor().Id == id) return;

    if (auto *oldCentral = takeCentralWidget()) delete oldCentral;
    if (m_RuntimeStatus)
    {
        statusBar()->removeWidget(m_RuntimeStatus);
        delete m_RuntimeStatus;
        m_RuntimeStatus = nullptr;
    }
    m_Viewport = nullptr;
    m_World.reset();
    try
    {
        m_World = WorldCatalog::Create(id, m_Profiling);
        BuildWorldUi();
    }
    catch (const std::exception &exception)
    {
        QMessageBox::critical(this, "Testbed initialization", exception.what());
        return;
    }

    auto *worldMenu = menuBar()->actions().front()->menu()->actions().front()->menu();
    for (auto *action : worldMenu->actions()) action->setChecked(action->data().toString().toStdString() == id);
    setWindowTitle(QString{"UnrealVoxelSim Qt Testbed - %1"}.arg(
        QString::fromUtf8(m_World->Descriptor().DisplayName.data(),
                          static_cast<qsizetype>(m_World->Descriptor().DisplayName.size()))));
    m_SimulationClock.restart();
}

void Window::BuildWorldUi()
{
    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    auto *toolbar = new QHBoxLayout;
    auto *tool = new QComboBox(central);
    auto *material = new QComboBox(central);
    auto *size = new QSpinBox(central);
    auto *renderDistance = new QSpinBox(central);
    auto *population = new QSpinBox(central);
    auto *simulationRate = new QComboBox(central);
    auto *singleStep = new QPushButton("Step", central);
    m_RuntimeStatus = new QLabel(statusBar());

    tool->addItem("Select", static_cast<int>(Tool::Select));
    tool->addItem("Fill", static_cast<int>(Tool::Fill));
    tool->addItem("Erase", static_cast<int>(Tool::Erase));
    tool->addItem("Navigate", static_cast<int>(Tool::Navigate));
    simulationRate->addItems({"Paused", "0.5x", "1x", "2x", "10x", "100x"});
    simulationRate->setCurrentIndex(2);
    material->addItem("Dirt", Voxel::Solid::Api::StandardMaterials::Dirt.Value());
    material->addItem("Grass", Voxel::Solid::Api::StandardMaterials::Grass.Value());
    material->addItem("Stone", Voxel::Solid::Api::StandardMaterials::Stone.Value());
    size->setRange(1, 9);
    size->setValue(1);
    size->setSuffix(" cells");
    renderDistance->setRange(16, 512);
    renderDistance->setSingleStep(16);
    renderDistance->setValue(80);
    renderDistance->setSuffix(" cells");
    population->setRange(
        0, static_cast<int>(std::min<std::size_t>(m_World->MaximumPopulation(),
                                                 static_cast<std::size_t>(std::numeric_limits<int>::max()))));
    population->setSingleStep(100);
    population->setValue(static_cast<int>(m_World->TargetPopulation()));
    population->setSuffix(" pawns");

    toolbar->addWidget(new QLabel("Tool:", central));
    toolbar->addWidget(tool);
    toolbar->addWidget(new QLabel("Solid:", central));
    toolbar->addWidget(material);
    toolbar->addWidget(new QLabel("Size:", central));
    toolbar->addWidget(size);
    toolbar->addWidget(new QLabel("Render:", central));
    toolbar->addWidget(renderDistance);
    toolbar->addWidget(new QLabel("Population:", central));
    toolbar->addWidget(population);
    toolbar->addWidget(new QLabel("Simulation:", central));
    toolbar->addWidget(simulationRate);
    toolbar->addWidget(singleStep);
    toolbar->addStretch(1);

    m_Viewport = new Viewport(*m_World, m_Profiling, central);
    m_Viewport->SetDiagnosticsSink([runtimeStatus = m_RuntimeStatus](const QString &text) {
        runtimeStatus->setText(text);
    });
    layout->addLayout(toolbar);
    layout->addWidget(m_Viewport, 1);
    setCentralWidget(central);
    statusBar()->addPermanentWidget(m_RuntimeStatus, 1);

    connect(tool, &QComboBox::currentIndexChanged, this, [this, tool](const int index) {
        m_Viewport->SetTool(static_cast<Tool>(tool->itemData(index).toInt()));
    });
    connect(material, &QComboBox::currentIndexChanged, this, [this, material](const int index) {
        m_Viewport->SetMaterial(Voxel::Solid::Api::MaterialId{material->itemData(index).toUInt()});
    });
    connect(size, &QSpinBox::valueChanged, this, [this](const int value) { m_Viewport->SetBrushSize(value); });
    connect(renderDistance, &QSpinBox::valueChanged, this,
            [this](const int value) { m_Viewport->SetRenderDistance(value); });
    connect(population, &QSpinBox::valueChanged, this, [this](const int value) {
        m_World->SetTargetPopulation(static_cast<std::size_t>(value));
    });
    connect(simulationRate, &QComboBox::currentIndexChanged, this,
            [this](const int index) { m_World->Pacer().SetRate(Rates[static_cast<std::size_t>(index)]); });
    connect(singleStep, &QPushButton::clicked, this, [this] {
        if (!m_World->Stepper().Step(Simulation::Api::TickCount{1}))
            m_Viewport->ReportStatus("Simulation tick overflow");
    });
}

}
