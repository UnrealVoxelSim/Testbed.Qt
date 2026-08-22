#pragma once

#include "BrushMode.h"

#include "UnrealVoxelSim/Events/Api/Subscription.h"
#include "UnrealVoxelSim/Ecs/Api/EntityId.h"
#include "UnrealVoxelSim/Movement/Api/IReader.h"
#include "UnrealVoxelSim/Navigation/Api/ICommandSink.h"
#include "UnrealVoxelSim/Navigation/Api/IExecutionReader.h"
#include "UnrealVoxelSim/Simulation/Api/IStepper.h"
#include "UnrealVoxelSim/Voxel/Api/IBounds.h"
#include "UnrealVoxelSim/Voxel/Api/Offset.h"
#include "UnrealVoxelSim/Voxel/Rendering/Api/Mesh.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/IChangeSource.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/ICommandSink.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/IReader.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/IRegionReader.h"
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
    Viewport(const UnrealVoxelSim::Voxel::Api::IBounds &bounds,
             const UnrealVoxelSim::Voxel::Solid::Api::IReader &reader,
             const UnrealVoxelSim::Voxel::Solid::Api::IRegionReader &regionReader,
             UnrealVoxelSim::Voxel::Solid::Api::ICommandSink &commands,
             UnrealVoxelSim::Voxel::Solid::Api::IChangeSource &changes,
             UnrealVoxelSim::Simulation::Api::IStepper &simulation,
             UnrealVoxelSim::Navigation::Api::ICommandSink &navigationCommands,
             const UnrealVoxelSim::Navigation::Api::IExecutionReader &navigationExecutions,
             const UnrealVoxelSim::Movement::Api::IReader &movement, UnrealVoxelSim::Ecs::Api::EntityId pawn,
             QWidget *parent = nullptr);
    ~Viewport() override;

    void SetBrushMode(BrushMode mode) noexcept;
    void SetBrushSize(int size) noexcept;
    void SetMaterial(UnrealVoxelSim::Voxel::Solid::Api::MaterialId material) noexcept;
    void SetRenderDistance(int distance) noexcept;
    void SetDiagnosticsSink(std::function<void(const QString &)> sink);
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

    struct GpuVertex final
    {
        float X{};
        float Y{};
        float Z{};
        float NormalX{};
        float NormalY{};
        float NormalZ{};
        std::uint32_t Surface{};
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
    [[nodiscard]] std::optional<Hit> Raycast(const QPoint &screenPosition) const;

    void EnsureResidency();
    void MarkDirty(UnrealVoxelSim::Voxel::Api::Region region);
    void ProcessJobs();
    void ScheduleJobs();
    void Upload(Tile &tile, const UnrealVoxelSim::Voxel::Rendering::Api::Mesh &mesh);
    void DestroyGpu(Tile &tile) noexcept;
    void ApplyBrush(const QPoint &screenPosition);
    void SubmitNavigation(const QPoint &screenPosition);
    void CreatePawnGpu();
    void PublishDiagnostics(std::size_t visibleTiles, std::size_t drawCalls, std::size_t triangles);

    const UnrealVoxelSim::Voxel::Api::IBounds &Bounds_;
    const UnrealVoxelSim::Voxel::Solid::Api::IReader &Reader_;
    const UnrealVoxelSim::Voxel::Solid::Api::IRegionReader &RegionReader_;
    UnrealVoxelSim::Voxel::Solid::Api::ICommandSink &Commands_;
    UnrealVoxelSim::Simulation::Api::IStepper &Simulation_;
    UnrealVoxelSim::Navigation::Api::ICommandSink &NavigationCommands_;
    const UnrealVoxelSim::Navigation::Api::IExecutionReader &NavigationExecutions_;
    const UnrealVoxelSim::Movement::Api::IReader &Movement_;
    UnrealVoxelSim::Ecs::Api::EntityId Pawn_;
    UnrealVoxelSim::Voxel::Solid::Rendering::Sampler Sampler_;
    std::unique_ptr<QOpenGLShaderProgram> Program_;
    std::unordered_map<TileKey, Tile, TileKeyHash> Tiles_;
    std::deque<TileKey> Dirty_;
    std::vector<Job> Jobs_;
    Tile PawnGpu_;
    std::optional<TileKey> ResidencyCenter_;
    std::int32_t ResidencyRadius_{};
    std::unordered_set<int> Keys_;
    QVector3D Camera_{40.0F, -48.0F, 35.0F};
    float Yaw_{130.0F};
    float Pitch_{-30.0F};
    QPoint LastMouse_;
    std::optional<UnrealVoxelSim::Voxel::Api::Position> LastBrushCell_;
    BrushMode BrushMode_{BrushMode::Fill};
    UnrealVoxelSim::Voxel::Solid::Api::MaterialId Material_{1};
    int BrushSize_{1};
    std::int32_t RenderDistance_{DefaultRenderDistance};
    bool Looking_{};
    bool Painting_{};
    QElapsedTimer MovementClock_;
    QElapsedTimer FrameClock_;
    std::size_t FrameCount_{};
    std::uint64_t NavigationSequence_{};
    std::uint64_t NavigationExecution_{};
    std::uint64_t SolidSequence_{};
    QString Status_{"Ready"};
    std::function<void(const QString &)> DiagnosticsSink_;
    UnrealVoxelSim::Events::Api::Subscription ChangesSubscription_;
};

} // namespace UnrealVoxelSim::Testbed::Qt
