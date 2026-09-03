#pragma once

#include "UnrealVoxelSim/Profiling/Api/IRecorder.h"
#include "UnrealVoxelSim/Testbed/World.h"

#include <QMainWindow>
#include <QElapsedTimer>

#include <memory>
#include <string_view>

class QLabel;

namespace UnrealVoxelSim::Testbed::Qt
{

class Viewport;
class Window final : public QMainWindow
{
  public:
    Window(std::string_view initialWorld, UnrealVoxelSim::Profiling::Api::IRecorder &profiling,
           QWidget *parent = nullptr);

    [[nodiscard]] UnrealVoxelSim::Testbed::World &CurrentWorld() noexcept;
    [[nodiscard]] bool TextureResourcesReady() const noexcept;

  private:
    void LoadWorld(std::string_view id);
    void BuildWorldUi();

    UnrealVoxelSim::Profiling::Api::IRecorder &m_Profiling;
    std::unique_ptr<UnrealVoxelSim::Testbed::World> m_World;
    QElapsedTimer m_SimulationClock;
    Viewport *m_Viewport{};
    QLabel *m_RuntimeStatus{};
};

}
