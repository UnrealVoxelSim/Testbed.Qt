#pragma once

#include "Tool.h"

#include "UnrealVoxelSim/Testbed/World.h"

#include "UnrealVoxelSim/Events/Api/Subscription.h"
#include "UnrealVoxelSim/Ecs/Api/EntityId.h"
#include "UnrealVoxelSim/Profiling/Api/IRecorder.h"
#include "UnrealVoxelSim/Simulation/Api/TickCount.h"
#include "UnrealVoxelSim/Voxel/Api/Offset.h"
#include "UnrealVoxelSim/Voxel/Rendering/Api/Mesh.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/IChangeSource.h"
#include "UnrealVoxelSim/Voxel/Solid/Rendering/BuildError.h"
#include "UnrealVoxelSim/Voxel/Solid/Rendering/Sampler.h"

#include <QElapsedTimer>
#include <QMatrix4x4>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include <QPoint>
#include <QString>
#include <QVector3D>

#include <cstddef>
#include <cstdint>
#include <compare>
#include <deque>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QKeyEvent;
class QMouseEvent;
class QOpenGLShaderProgram;

namespace UnrealVoxelSim::Testbed::Qt
{

class Viewport final : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core
{
  public:
    Viewport(UnrealVoxelSim::Testbed::World &world, UnrealVoxelSim::Profiling::Api::IRecorder &profiling,
             QWidget *parent = nullptr);
    ~Viewport() override;

    void SetTool(Tool tool) noexcept;
    void SetBrushSize(int size) noexcept;
    void SetMaterial(UnrealVoxelSim::Voxel::Solid::Api::MaterialId material) noexcept;
    void SetRenderDistance(int distance) noexcept;
    [[nodiscard]] bool TextureResourcesReady() const noexcept;
    void SetDiagnosticsSink(std::function<void(const QString &)> sink);
    void SetSimulationBacklog(UnrealVoxelSim::Simulation::Api::TickCount pending) noexcept;
    void ReportStatus(QString status);
    void Tick();

  protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

  private:
    struct TileKey final
    {
        std::int32_t X{};
        std::int32_t Y{};
        std::int32_t Z{};

        auto operator<=>(const TileKey &) const = default;
    };

    struct TileKeyHash final
    {
        [[nodiscard]] std::size_t operator()(const TileKey &key) const noexcept;
    };

    struct Tile final
    {
        std::uint64_t Generation{1};
        std::uint64_t UploadedGeneration{};
        unsigned int VertexArray{};
        unsigned int VertexBuffer{};
        unsigned int IndexBuffer{};
        int IndexCount{};
        bool Queued{};
    };

    struct Job final
    {
        TileKey Key;
        std::uint64_t Generation{};
        std::future<std::expected<UnrealVoxelSim::Voxel::Rendering::Api::Mesh,
                                  UnrealVoxelSim::Voxel::Solid::Rendering::BuildError>>
            Future;
    };

    struct Hit final
    {
        UnrealVoxelSim::Voxel::Api::Position Cell;
        UnrealVoxelSim::Voxel::Api::Offset Normal;
    };

    struct Ray final
    {
        QVector3D Origin;
        QVector3D Direction;
    };

    struct GpuVertex final
    {
        float X{};
        float Y{};
        float Z{};
        float NormalX{};
        float NormalY{};
        float NormalZ{};
        std::uint32_t Surface{};
        float U{};
        float V{};
    };

    struct GpuOffset final
    {
        float X{};
        float Y{};
        float Z{};
    };

    static constexpr std::int32_t TileEdge = 16;
    static constexpr std::int32_t MinimumRenderDistance = TileEdge;
    static constexpr std::int32_t MaximumRenderDistance = 512;
    static constexpr std::int32_t DefaultRenderDistance = 80;

    [[nodiscard]] static std::int32_t FloorDiv(std::int32_t value) noexcept;
    [[nodiscard]] UnrealVoxelSim::Voxel::Api::Region TileRegion(TileKey key) const noexcept;
    [[nodiscard]] QVector3D Forward() const noexcept;
    [[nodiscard]] QMatrix4x4 ViewProjection() const;
    [[nodiscard]] bool IsVisible(UnrealVoxelSim::Voxel::Api::Region region, const QMatrix4x4 &viewProjection) const;
    [[nodiscard]] std::optional<Ray> ScreenRay(const QPoint &screenPosition) const;
    [[nodiscard]] std::optional<Hit> Raycast(const QPoint &screenPosition) const;

    void EnsureResidency();
    void MarkDirty(UnrealVoxelSim::Voxel::Api::Region region);
    void ProcessJobs();
    void ScheduleJobs();
    void Upload(Tile &tile, const UnrealVoxelSim::Voxel::Rendering::Api::Mesh &mesh);
    void DestroyGpu(Tile &tile) noexcept;
    void CreateTextureResources();
    void DestroyTextureResources() noexcept;
    void ApplyBrush(const QPoint &screenPosition);
    void SelectPawn(const QPoint &screenPosition);
    void SubmitNavigation(const QPoint &screenPosition);
    void CreatePawnGpu();
    void PublishDiagnostics(std::size_t visibleTiles, std::size_t visiblePawns, std::size_t drawCalls,
                            std::size_t triangles);

    UnrealVoxelSim::Testbed::World &m_World;
    UnrealVoxelSim::Profiling::Api::IRecorder &m_Profiling;
    UnrealVoxelSim::Voxel::Solid::Rendering::Sampler m_Sampler;
    std::unique_ptr<QOpenGLShaderProgram> m_Program;
    std::unordered_map<TileKey, Tile, TileKeyHash> m_Tiles;
    std::deque<TileKey> m_Dirty;
    std::vector<Job> m_Jobs;
    Tile m_PawnGpu;
    unsigned int m_PawnInstanceBuffer{};
    unsigned int m_TileTextures{};
    unsigned int m_SurfaceTableTexture{};
    unsigned int m_SurfaceTableBuffer{};
    std::optional<TileKey> m_ResidencyCenter;
    std::int32_t m_ResidencyRadius{};
    std::unordered_set<int> m_Keys;
    QVector3D m_Camera{40.0F, -48.0F, 35.0F};
    float m_Yaw{130.0F};
    float m_Pitch{-30.0F};
    QPoint m_LastMouse;
    std::optional<UnrealVoxelSim::Voxel::Api::Position> m_LastBrushCell;
    Tool m_Tool{Tool::Select};
    std::optional<UnrealVoxelSim::Ecs::Api::EntityId> m_SelectedPawn;
    UnrealVoxelSim::Voxel::Solid::Api::MaterialId m_Material{1};
    int m_BrushSize{1};
    std::int32_t m_RenderDistance{DefaultRenderDistance};
    bool m_Looking{};
    bool m_Painting{};
    QElapsedTimer m_MovementClock;
    QElapsedTimer m_FrameClock;
    std::size_t m_FrameCount{};
    UnrealVoxelSim::Simulation::Api::TickCount m_PendingSimulationTicks{};
    QString m_Status{"Ready"};
    std::function<void(const QString &)> m_DiagnosticsSink;
    UnrealVoxelSim::Events::Api::Subscription m_ChangesSubscription;
};

}
