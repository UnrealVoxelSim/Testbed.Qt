#include "Viewport.h"

#include "UnrealVoxelSim/Voxel/Solid/Api/Cell.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/Changed.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/Placement.h"
#include "UnrealVoxelSim/Voxel/Solid/Rendering/GreedyMesher.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLShader>
#include <QOpenGLShaderProgram>
#include <QVector4D>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <thread>
#include <utility>

namespace UnrealVoxelSim::Testbed::Qt
{

namespace
{

constexpr auto VertexShader = R"(
#version 330 core
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 vertexNormal;
layout(location = 2) in uint surface;

uniform mat4 viewProjection;
uniform vec3 modelOffset;

out vec3 normal;
flat out uint surfaceId;

void main()
{
    gl_Position = viewProjection * vec4(position + modelOffset, 1.0);
    normal = vertexNormal;
    surfaceId = surface;
}
)";

constexpr auto FragmentShader = R"(
#version 330 core
in vec3 normal;
flat in uint surfaceId;
out vec4 color;

vec3 surfaceColor(uint id)
{
    if (id == 1u) return vec3(0.42, 0.25, 0.10);
    if (id == 2u) return vec3(0.18, 0.62, 0.20);
    if (id == 3u) return vec3(0.48, 0.50, 0.54);
    if (id == 4u) return vec3(0.95, 0.72, 0.08);
    return vec3(0.85, 0.15, 0.75);
}

void main()
{
    vec3 lightDirection = normalize(vec3(0.45, -0.35, 0.82));
    float illumination = 0.32 + 0.68 * max(dot(normalize(normal), lightDirection), 0.0);
    color = vec4(surfaceColor(surfaceId) * illumination, 1.0);
}
)";

} // namespace

Viewport::Viewport(const UnrealVoxelSim::Voxel::Api::IBounds &bounds,
                   const UnrealVoxelSim::Voxel::Solid::Api::IReader &reader,
                   const UnrealVoxelSim::Voxel::Solid::Api::IRegionReader &regionReader,
                   UnrealVoxelSim::Voxel::Solid::Api::ICommandSink &commands,
                   UnrealVoxelSim::Voxel::Solid::Api::IChangeSource &changes,
                   UnrealVoxelSim::Simulation::Api::IStepper &simulation,
                   UnrealVoxelSim::Navigation::Api::ICommandSink &navigationCommands,
                   const UnrealVoxelSim::Navigation::Api::IExecutionReader &navigationExecutions,
                   const UnrealVoxelSim::Movement::Api::IReader &movement,
                   const UnrealVoxelSim::Ecs::Api::EntityId pawn, QWidget *parent)
    : QOpenGLWidget(parent), Bounds_(bounds), Reader_(reader), RegionReader_(regionReader), Commands_(commands),
      Simulation_(simulation), NavigationCommands_(navigationCommands), NavigationExecutions_(navigationExecutions),
      Movement_(movement), Pawn_(pawn), Sampler_(bounds, regionReader)
{
    setFocusPolicy(::Qt::StrongFocus);
    setMouseTracking(true);
    MovementClock_.start();
    FrameClock_.start();
    ChangesSubscription_ =
        changes.Subscribe([this](const UnrealVoxelSim::Voxel::Solid::Api::Changed &changed) noexcept {
            for (const auto region : changed.Regions)
            {
                MarkDirty(region);
            }
        });
}

Viewport::~Viewport()
{
    ChangesSubscription_.Reset();
    for (auto &job : Jobs_)
    {
        job.Future.wait();
    }
    if (isValid())
    {
        makeCurrent();
        for (auto &[key, tile] : Tiles_)
        {
            static_cast<void>(key);
            DestroyGpu(tile);
        }
        DestroyGpu(PawnGpu_);
        Program_.reset();
        doneCurrent();
    }
}

void Viewport::SetBrushMode(const BrushMode mode) noexcept
{
    BrushMode_ = mode;
    LastBrushCell_.reset();
}

void Viewport::SetBrushSize(const int size) noexcept
{
    BrushSize_ = std::clamp(size, 1, 9);
    LastBrushCell_.reset();
}

void Viewport::SetMaterial(const UnrealVoxelSim::Voxel::Solid::Api::MaterialId material) noexcept
{
    if (material.IsValid())
    {
        Material_ = material;
    }
}

void Viewport::SetRenderDistance(const int distance) noexcept
{
    RenderDistance_ = std::clamp(distance, MinimumRenderDistance, MaximumRenderDistance);
}

void Viewport::SetDiagnosticsSink(std::function<void(const QString &)> sink)
{
    DiagnosticsSink_ = std::move(sink);
}

void Viewport::SetSimulationBacklog(const UnrealVoxelSim::Simulation::Api::TickCount pending) noexcept
{
    PendingSimulationTicks_ = pending;
}

void Viewport::ReportStatus(QString status)
{
    Status_ = std::move(status);
}

void Viewport::Tick()
{
    const auto elapsedNanoseconds = MovementClock_.nsecsElapsed();
    MovementClock_.restart();
    const auto elapsed = std::min(static_cast<float>(elapsedNanoseconds) / 1'000'000'000.0F, 0.1F);
    const auto speed = Keys_.contains(::Qt::Key_Shift) ? 45.0F : 18.0F;
    const auto forward = Forward();
    auto right = QVector3D::crossProduct(forward, QVector3D{0.0F, 0.0F, 1.0F});
    if (!right.isNull())
    {
        right.normalize();
    }
    QVector3D movement;
    if (Keys_.contains(::Qt::Key_W))
        movement += forward;
    if (Keys_.contains(::Qt::Key_S))
        movement -= forward;
    if (Keys_.contains(::Qt::Key_D))
        movement += right;
    if (Keys_.contains(::Qt::Key_A))
        movement -= right;
    if (Keys_.contains(::Qt::Key_E))
        movement += QVector3D{0.0F, 0.0F, 1.0F};
    if (Keys_.contains(::Qt::Key_Q))
        movement -= QVector3D{0.0F, 0.0F, 1.0F};
    if (!movement.isNull())
    {
        Camera_ += movement.normalized() * speed * elapsed;
    }
    update();
}

void Viewport::initializeGL()
{
    initializeOpenGLFunctions();
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    Program_ = std::make_unique<QOpenGLShaderProgram>();
    if (!Program_->addShaderFromSourceCode(QOpenGLShader::Vertex, VertexShader) ||
        !Program_->addShaderFromSourceCode(QOpenGLShader::Fragment, FragmentShader) || !Program_->link())
    {
        Status_ = QString{"Shader initialization failed: %1"}.arg(Program_->log());
    }
    else
    {
        CreatePawnGpu();
    }
}

void Viewport::paintGL()
{
    EnsureResidency();
    ProcessJobs();
    ScheduleJobs();

    glClearColor(0.52F, 0.72F, 0.92F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!Program_ || !Program_->isLinked())
    {
        return;
    }

    const auto viewProjection = ViewProjection();
    Program_->bind();
    Program_->setUniformValue("viewProjection", viewProjection);
    Program_->setUniformValue("modelOffset", QVector3D{});
    std::size_t visibleTiles{};
    std::size_t drawCalls{};
    std::size_t triangles{};
    for (const auto &[key, tile] : Tiles_)
    {
        if (tile.IndexCount == 0 || !IsVisible(TileRegion(key), viewProjection))
        {
            continue;
        }
        ++visibleTiles;
        ++drawCalls;
        triangles += static_cast<std::size_t>(tile.IndexCount) / 3;
        glBindVertexArray(tile.VertexArray);
        glDrawElements(GL_TRIANGLES, tile.IndexCount, GL_UNSIGNED_INT, nullptr);
    }
    const auto pawn = Movement_.Read(Pawn_);
    if (pawn && PawnGpu_.IndexCount != 0)
    {
        Program_->setUniformValue(
            "modelOffset", QVector3D{static_cast<float>(pawn->Location.X.ToDouble()),
                                     static_cast<float>(pawn->Location.Y.ToDouble()),
                                     static_cast<float>(pawn->Location.Z.ToDouble())});
        glBindVertexArray(PawnGpu_.VertexArray);
        glDrawElements(GL_TRIANGLES, PawnGpu_.IndexCount, GL_UNSIGNED_INT, nullptr);
        ++drawCalls;
        triangles += static_cast<std::size_t>(PawnGpu_.IndexCount) / 3;
    }
    glBindVertexArray(0);
    Program_->release();
    PublishDiagnostics(visibleTiles, drawCalls, triangles);
}

void Viewport::resizeGL(const int width, const int height)
{
    glViewport(0, 0, width, height);
}

void Viewport::keyPressEvent(QKeyEvent *event)
{
    if (!event->isAutoRepeat())
    {
        Keys_.insert(event->key());
    }
    QOpenGLWidget::keyPressEvent(event);
}

void Viewport::keyReleaseEvent(QKeyEvent *event)
{
    if (!event->isAutoRepeat())
    {
        Keys_.erase(event->key());
    }
    QOpenGLWidget::keyReleaseEvent(event);
}

void Viewport::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    LastMouse_ = event->position().toPoint();
    if (event->button() == ::Qt::RightButton)
    {
        Looking_ = true;
    }
    if (event->button() == ::Qt::LeftButton)
    {
        Painting_ = true;
        LastBrushCell_.reset();
        ApplyBrush(event->position().toPoint());
    }
}

void Viewport::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == ::Qt::RightButton)
    {
        Looking_ = false;
    }
    if (event->button() == ::Qt::LeftButton)
    {
        Painting_ = false;
        LastBrushCell_.reset();
    }
}

void Viewport::mouseMoveEvent(QMouseEvent *event)
{
    const auto position = event->position().toPoint();
    if (Looking_)
    {
        const auto delta = position - LastMouse_;
        Yaw_ -= static_cast<float>(delta.x()) * 0.18F;
        Pitch_ = std::clamp(Pitch_ - static_cast<float>(delta.y()) * 0.18F, -89.0F, 89.0F);
    }
    if (Painting_)
    {
        ApplyBrush(position);
    }
    LastMouse_ = position;
}

std::size_t Viewport::TileKeyHash::operator()(const TileKey &key) const noexcept
{
    const auto x = static_cast<std::uint32_t>(key.X);
    const auto y = static_cast<std::uint32_t>(key.Y);
    const auto z = static_cast<std::uint32_t>(key.Z);
    return static_cast<std::size_t>(x * 73856093U ^ y * 19349663U ^ z * 83492791U);
}

std::int32_t Viewport::FloorDiv(const std::int32_t value) noexcept
{
    if (value >= 0)
    {
        return value / TileEdge;
    }
    return static_cast<std::int32_t>((static_cast<std::int64_t>(value) - (TileEdge - 1)) / TileEdge);
}

UnrealVoxelSim::Voxel::Api::Region Viewport::TileRegion(const TileKey key) const noexcept
{
    const auto bounds = Bounds_.Bounds();
    const auto minimumX = static_cast<std::int64_t>(key.X) * TileEdge;
    const auto minimumY = static_cast<std::int64_t>(key.Y) * TileEdge;
    const auto minimumZ = static_cast<std::int64_t>(key.Z) * TileEdge;
    const auto clamp = [](const std::int64_t value) {
        return static_cast<std::int32_t>(
            std::clamp(value, static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()),
                       static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())));
    };
    return {{std::max(bounds.Min.X, clamp(minimumX)), std::max(bounds.Min.Y, clamp(minimumY)),
             std::max(bounds.Min.Z, clamp(minimumZ))},
            {std::min(bounds.Max.X, clamp(minimumX + TileEdge)), std::min(bounds.Max.Y, clamp(minimumY + TileEdge)),
             std::min(bounds.Max.Z, clamp(minimumZ + TileEdge))}};
}

QVector3D Viewport::Forward() const noexcept
{
    constexpr auto DegreesToRadians = 3.14159265358979323846F / 180.0F;
    const auto yaw = Yaw_ * DegreesToRadians;
    const auto pitch = Pitch_ * DegreesToRadians;
    return QVector3D{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)}.normalized();
}

QMatrix4x4 Viewport::ViewProjection() const
{
    QMatrix4x4 projection;
    projection.perspective(65.0F, static_cast<float>(std::max(width(), 1)) / static_cast<float>(std::max(height(), 1)),
                           0.1F, static_cast<float>(RenderDistance_ + TileEdge * 2));
    QMatrix4x4 view;
    view.lookAt(Camera_, Camera_ + Forward(), QVector3D{0.0F, 0.0F, 1.0F});
    return projection * view;
}

bool Viewport::IsVisible(const UnrealVoxelSim::Voxel::Api::Region region, const QMatrix4x4 &viewProjection) const
{
    std::array<QVector4D, 8> corners;
    std::size_t index{};
    for (const auto x : {region.Min.X, region.Max.X})
    {
        for (const auto y : {region.Min.Y, region.Max.Y})
        {
            for (const auto z : {region.Min.Z, region.Max.Z})
            {
                corners[index++] = viewProjection *
                                   QVector4D{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), 1.0F};
            }
        }
    }

    const auto outside = [&corners](const auto predicate) { return std::ranges::all_of(corners, predicate); };
    return !outside([](const QVector4D &point) { return point.x() < -point.w(); }) &&
           !outside([](const QVector4D &point) { return point.x() > point.w(); }) &&
           !outside([](const QVector4D &point) { return point.y() < -point.w(); }) &&
           !outside([](const QVector4D &point) { return point.y() > point.w(); }) &&
           !outside([](const QVector4D &point) { return point.z() < -point.w(); }) &&
           !outside([](const QVector4D &point) { return point.z() > point.w(); });
}

void Viewport::EnsureResidency()
{
    const auto renderRadius = (RenderDistance_ + TileEdge - 1) / TileEdge;
    const TileKey center{FloorDiv(static_cast<std::int32_t>(std::floor(Camera_.x()))),
                         FloorDiv(static_cast<std::int32_t>(std::floor(Camera_.y()))),
                         FloorDiv(static_cast<std::int32_t>(std::floor(Camera_.z())))};
    if (ResidencyCenter_ == center && ResidencyRadius_ == renderRadius)
    {
        return;
    }
    ResidencyCenter_ = center;
    ResidencyRadius_ = renderRadius;

    std::unordered_set<TileKey, TileKeyHash> desired;
    for (auto z = -renderRadius; z <= renderRadius; ++z)
    {
        for (auto y = -renderRadius; y <= renderRadius; ++y)
        {
            for (auto x = -renderRadius; x <= renderRadius; ++x)
            {
                if (x * x + y * y + z * z > renderRadius * renderRadius)
                {
                    continue;
                }
                const TileKey key{center.X + x, center.Y + y, center.Z + z};
                const auto region = TileRegion(key);
                if (!region.IsValid() || region.IsEmpty())
                {
                    continue;
                }
                desired.insert(key);
                const auto [iterator, inserted] = Tiles_.try_emplace(key);
                if (inserted)
                {
                    iterator->second.Queued = true;
                    Dirty_.push_back(key);
                }
            }
        }
    }

    for (auto iterator = Tiles_.begin(); iterator != Tiles_.end();)
    {
        if (!desired.contains(iterator->first))
        {
            DestroyGpu(iterator->second);
            iterator = Tiles_.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
}

void Viewport::MarkDirty(const UnrealVoxelSim::Voxel::Api::Region region)
{
    if (!region.IsValid() || region.IsEmpty())
    {
        return;
    }
    const auto bounds = Bounds_.Bounds();
    const UnrealVoxelSim::Voxel::Api::Region expanded{{region.Min.X > bounds.Min.X ? region.Min.X - 1 : region.Min.X,
                                                       region.Min.Y > bounds.Min.Y ? region.Min.Y - 1 : region.Min.Y,
                                                       region.Min.Z > bounds.Min.Z ? region.Min.Z - 1 : region.Min.Z},
                                                      {region.Max.X < bounds.Max.X ? region.Max.X + 1 : region.Max.X,
                                                       region.Max.Y < bounds.Max.Y ? region.Max.Y + 1 : region.Max.Y,
                                                       region.Max.Z < bounds.Max.Z ? region.Max.Z + 1 : region.Max.Z}};
    const TileKey minimum{FloorDiv(expanded.Min.X), FloorDiv(expanded.Min.Y), FloorDiv(expanded.Min.Z)};
    const TileKey maximum{FloorDiv(expanded.Max.X - 1), FloorDiv(expanded.Max.Y - 1), FloorDiv(expanded.Max.Z - 1)};
    for (auto z = minimum.Z; z <= maximum.Z; ++z)
    {
        for (auto y = minimum.Y; y <= maximum.Y; ++y)
        {
            for (auto x = minimum.X; x <= maximum.X; ++x)
            {
                const TileKey key{x, y, z};
                const auto iterator = Tiles_.find(key);
                if (iterator == Tiles_.end())
                {
                    continue;
                }
                ++iterator->second.Generation;
                if (!iterator->second.Queued)
                {
                    iterator->second.Queued = true;
                    Dirty_.push_back(key);
                }
            }
        }
    }
}

void Viewport::ProcessJobs()
{
    for (auto iterator = Jobs_.begin(); iterator != Jobs_.end();)
    {
        if (iterator->Future.wait_for(std::chrono::seconds{0}) != std::future_status::ready)
        {
            ++iterator;
            continue;
        }
        auto result = iterator->Future.get();
        const auto tile = Tiles_.find(iterator->Key);
        if (tile != Tiles_.end() && tile->second.Generation == iterator->Generation)
        {
            if (result)
            {
                Upload(tile->second, *result);
                tile->second.UploadedGeneration = iterator->Generation;
            }
            else
            {
                Status_ = "Meshing failed";
            }
        }
        iterator = Jobs_.erase(iterator);
    }
}

void Viewport::ScheduleJobs()
{
    const auto hardwareThreads = std::max(std::thread::hardware_concurrency(), 2U);
    const auto maximumJobs = static_cast<std::size_t>(std::min(hardwareThreads - 1, 4U));
    std::size_t captured{};
    while (!Dirty_.empty() && Jobs_.size() < maximumJobs && captured < 2)
    {
        const auto key = Dirty_.front();
        Dirty_.pop_front();
        const auto tile = Tiles_.find(key);
        if (tile == Tiles_.end())
        {
            continue;
        }
        tile->second.Queued = false;
        const auto generation = tile->second.Generation;
        auto snapshot = Sampler_.Capture(TileRegion(key));
        if (!snapshot)
        {
            Status_ = "Voxel snapshot capture failed";
            tile->second.UploadedGeneration = generation;
            continue;
        }
        Jobs_.push_back(
            Job{key, generation, std::async(std::launch::async, [snapshot = std::move(*snapshot)]() mutable {
                    return UnrealVoxelSim::Voxel::Solid::Rendering::GreedyMesher{}.Build(snapshot);
                })});
        ++captured;
    }
}

void Viewport::Upload(Tile &tile, const UnrealVoxelSim::Voxel::Rendering::Api::Mesh &mesh)
{
    DestroyGpu(tile);
    if (mesh.Indices.empty())
    {
        return;
    }

    std::vector<GpuVertex> vertices;
    vertices.reserve(mesh.Vertices.size());
    for (const auto vertex : mesh.Vertices)
    {
        vertices.push_back(
            {static_cast<float>(mesh.Bounds.Min.X + vertex.X), static_cast<float>(mesh.Bounds.Min.Y + vertex.Y),
             static_cast<float>(mesh.Bounds.Min.Z + vertex.Z), static_cast<float>(vertex.NormalX),
             static_cast<float>(vertex.NormalY), static_cast<float>(vertex.NormalZ), vertex.Surface.Value()});
    }

    glGenVertexArrays(1, &tile.VertexArray);
    glGenBuffers(1, &tile.VertexBuffer);
    glGenBuffers(1, &tile.IndexBuffer);
    glBindVertexArray(tile.VertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, tile.VertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(GpuVertex)), vertices.data(),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, tile.IndexBuffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.Indices.size() * sizeof(std::uint32_t)),
                 mesh.Indices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                          reinterpret_cast<void *>(offsetof(GpuVertex, X)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                          reinterpret_cast<void *>(offsetof(GpuVertex, NormalX)));
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(GpuVertex),
                           reinterpret_cast<void *>(offsetof(GpuVertex, Surface)));
    glBindVertexArray(0);
    tile.IndexCount = static_cast<int>(mesh.Indices.size());
}

void Viewport::DestroyGpu(Tile &tile) noexcept
{
    if (tile.IndexBuffer != 0)
        glDeleteBuffers(1, &tile.IndexBuffer);
    if (tile.VertexBuffer != 0)
        glDeleteBuffers(1, &tile.VertexBuffer);
    if (tile.VertexArray != 0)
        glDeleteVertexArrays(1, &tile.VertexArray);
    tile.IndexBuffer = 0;
    tile.VertexBuffer = 0;
    tile.VertexArray = 0;
    tile.IndexCount = 0;
}

std::optional<Viewport::Hit> Viewport::Raycast(const QPoint &screenPosition) const
{
    bool invertible{};
    const auto inverse = ViewProjection().inverted(&invertible);
    if (!invertible || width() <= 0 || height() <= 0)
    {
        return std::nullopt;
    }
    const auto x = 2.0F * static_cast<float>(screenPosition.x()) / static_cast<float>(width()) - 1.0F;
    const auto y = 1.0F - 2.0F * static_cast<float>(screenPosition.y()) / static_cast<float>(height());
    auto nearPoint = inverse * QVector4D{x, y, -1.0F, 1.0F};
    auto farPoint = inverse * QVector4D{x, y, 1.0F, 1.0F};
    nearPoint /= nearPoint.w();
    farPoint /= farPoint.w();
    const auto origin = nearPoint.toVector3D();
    const auto direction = (farPoint.toVector3D() - origin).normalized();

    std::array<std::int32_t, 3> cell{static_cast<std::int32_t>(std::floor(origin.x())),
                                     static_cast<std::int32_t>(std::floor(origin.y())),
                                     static_cast<std::int32_t>(std::floor(origin.z()))};
    const std::array<float, 3> originValues{origin.x(), origin.y(), origin.z()};
    const std::array<float, 3> directionValues{direction.x(), direction.y(), direction.z()};
    std::array<std::int32_t, 3> step{};
    std::array<float, 3> delta{};
    std::array<float, 3> maximum{};
    constexpr auto infinity = std::numeric_limits<float>::infinity();
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        step[axis] = directionValues[axis] > 0.0F ? 1 : (directionValues[axis] < 0.0F ? -1 : 0);
        delta[axis] = step[axis] == 0 ? infinity : std::abs(1.0F / directionValues[axis]);
        const auto boundary = static_cast<float>(cell[axis] + (step[axis] > 0 ? 1 : 0));
        maximum[axis] = step[axis] == 0 ? infinity : (boundary - originValues[axis]) / directionValues[axis];
    }

    UnrealVoxelSim::Voxel::Api::Offset normal{};
    float distance{};
    const auto bounds = Bounds_.Bounds();
    for (std::size_t iteration = 0; iteration < 768 && distance <= 256.0F; ++iteration)
    {
        const UnrealVoxelSim::Voxel::Api::Position position{cell[0], cell[1], cell[2]};
        if (bounds.Contains(position))
        {
            const auto value = Reader_.Read(position);
            if (value && !value->IsEmpty())
            {
                return Hit{position, normal};
            }
        }

        const auto axis = static_cast<std::size_t>(std::distance(maximum.begin(), std::ranges::min_element(maximum)));
        cell[axis] += step[axis];
        distance = maximum[axis];
        maximum[axis] += delta[axis];
        normal = {};
        if (axis == 0)
            normal.X = -step[axis];
        if (axis == 1)
            normal.Y = -step[axis];
        if (axis == 2)
            normal.Z = -step[axis];
    }
    return std::nullopt;
}

void Viewport::ApplyBrush(const QPoint &screenPosition)
{
    if (BrushMode_ == BrushMode::Navigate)
    {
        SubmitNavigation(screenPosition);
        return;
    }
    const auto hit = Raycast(screenPosition);
    if (!hit)
    {
        Status_ = "No solid voxel under cursor";
        return;
    }
    auto center = hit->Cell;
    if (BrushMode_ == BrushMode::Fill)
    {
        center.X += hit->Normal.X;
        center.Y += hit->Normal.Y;
        center.Z += hit->Normal.Z;
    }
    if (LastBrushCell_ == center)
    {
        return;
    }
    LastBrushCell_ = center;

    const auto bounds = Bounds_.Bounds();
    const auto before = (BrushSize_ - 1) / 2;
    const auto after = BrushSize_ / 2;
    const UnrealVoxelSim::Voxel::Api::Region region{
        {std::max(bounds.Min.X, center.X - before), std::max(bounds.Min.Y, center.Y - before),
         std::max(bounds.Min.Z, center.Z - before)},
        {std::min(bounds.Max.X, center.X + after + 1), std::min(bounds.Max.Y, center.Y + after + 1),
         std::min(bounds.Max.Z, center.Z + after + 1)}};
    if (!region.IsValid() || region.IsEmpty())
    {
        Status_ = "Brush is outside world bounds";
        return;
    }

    std::vector<UnrealVoxelSim::Voxel::Solid::Api::Cell> cells(*region.CellCount());
    if (!RegionReader_.ReadRegion(region, cells))
    {
        Status_ = "Brush region read failed";
        return;
    }

    std::vector<UnrealVoxelSim::Voxel::Api::Position> removals;
    std::vector<UnrealVoxelSim::Voxel::Solid::Api::Placement> placements;
    std::size_t index{};
    for (auto z = region.Min.Z; z < region.Max.Z; ++z)
    {
        for (auto y = region.Min.Y; y < region.Max.Y; ++y)
        {
            for (auto x = region.Min.X; x < region.Max.X; ++x)
            {
                const UnrealVoxelSim::Voxel::Api::Position position{x, y, z};
                const auto cell = cells[index++];
                if (BrushMode_ == BrushMode::Erase && !cell.IsEmpty())
                    removals.push_back(position);
                if (BrushMode_ == BrushMode::Fill && cell.IsEmpty())
                    placements.push_back({position, Material_});
            }
        }
    }

    if (BrushMode_ == BrushMode::Erase)
    {
        if (removals.empty())
            return;
        const UnrealVoxelSim::Voxel::Solid::Api::QueuedCommand command{
            UnrealVoxelSim::Voxel::Solid::Api::EraseCommand{
                {Simulation_.CurrentTick(), UnrealVoxelSim::Simulation::Api::CommandSourceId{2}, ++SolidSequence_},
                std::move(removals)}};
        const std::array commands{command};
        Status_ = Commands_.Submit(commands) ? "Erase command queued" : "Erase command rejected";
    }
    else
    {
        if (placements.empty())
            return;
        const UnrealVoxelSim::Voxel::Solid::Api::QueuedCommand command{
            UnrealVoxelSim::Voxel::Solid::Api::FillCommand{
                {Simulation_.CurrentTick(), UnrealVoxelSim::Simulation::Api::CommandSourceId{2}, ++SolidSequence_},
                std::move(placements)}};
        const std::array commands{command};
        Status_ = Commands_.Submit(commands) ? "Fill command queued" : "Fill command rejected";
    }
}

void Viewport::SubmitNavigation(const QPoint &screenPosition)
{
    const auto hit = Raycast(screenPosition);
    if (!hit)
    {
        Status_ = "No navigation destination under cursor";
        return;
    }
    auto foot = hit->Cell;
    foot.X += hit->Normal.X;
    foot.Y += hit->Normal.Y;
    foot.Z += hit->Normal.Z;
    constexpr auto one = UnrealVoxelSim::Movement::Api::Scalar::OneRaw;
    constexpr auto half = one / 2;
    const UnrealVoxelSim::Movement::Api::Position goal{
        UnrealVoxelSim::Movement::Api::Scalar::FromRaw(static_cast<std::int64_t>(foot.X) * one + half),
        UnrealVoxelSim::Movement::Api::Scalar::FromRaw(static_cast<std::int64_t>(foot.Y) * one + half),
        UnrealVoxelSim::Movement::Api::Scalar::FromRaw(static_cast<std::int64_t>(foot.Z) * one)};
    const UnrealVoxelSim::Navigation::Api::Command command{UnrealVoxelSim::Navigation::Api::Start{
        {Simulation_.CurrentTick(), UnrealVoxelSim::Simulation::Api::CommandSourceId{1}, ++NavigationSequence_}, Pawn_,
        UnrealVoxelSim::Navigation::Api::ExecutionId{++NavigationExecution_},
        {goal, UnrealVoxelSim::Movement::Api::Scalar::FromRaw(one / 4)}}};
    const std::array commands{command};
    const auto result = NavigationCommands_.Submit(commands);
    Status_ = result ? "Navigation execution queued" : "Navigation command rejected";
}

void Viewport::CreatePawnGpu()
{
    constexpr std::array vertices{
        GpuVertex{-0.45F, -0.45F, 0.0F, 0.0F, 0.0F, 1.0F, 4},
        GpuVertex{0.45F, -0.45F, 0.0F, 0.0F, 0.0F, 1.0F, 4},
        GpuVertex{0.45F, 0.45F, 0.0F, 0.0F, 0.0F, 1.0F, 4},
        GpuVertex{-0.45F, 0.45F, 0.0F, 0.0F, 0.0F, 1.0F, 4},
        GpuVertex{-0.45F, -0.45F, 1.8F, 0.0F, 0.0F, 1.0F, 4},
        GpuVertex{0.45F, -0.45F, 1.8F, 0.0F, 0.0F, 1.0F, 4},
        GpuVertex{0.45F, 0.45F, 1.8F, 0.0F, 0.0F, 1.0F, 4},
        GpuVertex{-0.45F, 0.45F, 1.8F, 0.0F, 0.0F, 1.0F, 4},
    };
    constexpr std::array<std::uint32_t, 36> indices{
        0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
        1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7,
    };
    glGenVertexArrays(1, &PawnGpu_.VertexArray);
    glGenBuffers(1, &PawnGpu_.VertexBuffer);
    glGenBuffers(1, &PawnGpu_.IndexBuffer);
    glBindVertexArray(PawnGpu_.VertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, PawnGpu_.VertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(GpuVertex)), vertices.data(),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, PawnGpu_.IndexBuffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)), indices.data(),
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), reinterpret_cast<void *>(offsetof(GpuVertex, X)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                          reinterpret_cast<void *>(offsetof(GpuVertex, NormalX)));
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(GpuVertex),
                           reinterpret_cast<void *>(offsetof(GpuVertex, Surface)));
    glBindVertexArray(0);
    PawnGpu_.IndexCount = static_cast<int>(indices.size());
}

void Viewport::PublishDiagnostics(const std::size_t visibleTiles, const std::size_t drawCalls,
                                  const std::size_t triangles)
{
    ++FrameCount_;
    const auto elapsed = FrameClock_.elapsed();
    if (elapsed < 500 || !DiagnosticsSink_)
    {
        return;
    }
    const auto framesPerSecond = static_cast<double>(FrameCount_) * 1000.0 / static_cast<double>(elapsed);
    QString navigation{"idle"};
    if (const auto execution = NavigationExecutions_.ReadExecution(Pawn_))
    {
        switch (execution->State)
        {
        case UnrealVoxelSim::Navigation::Api::ExecutionState::Planning: navigation = "planning"; break;
        case UnrealVoxelSim::Navigation::Api::ExecutionState::Following: navigation = "following"; break;
        case UnrealVoxelSim::Navigation::Api::ExecutionState::Replanning: navigation = "replanning"; break;
        case UnrealVoxelSim::Navigation::Api::ExecutionState::Arrived: navigation = "arrived"; break;
        case UnrealVoxelSim::Navigation::Api::ExecutionState::Unreachable: navigation = "unreachable"; break;
        case UnrealVoxelSim::Navigation::Api::ExecutionState::Cancelled: navigation = "cancelled"; break;
        }
    }
    DiagnosticsSink_(QString{"%1 FPS | tick %2 | lag %3 ticks | nav %4 | tiles %5/%6 | rebuild %7 queued, %8 active | draws %9 | triangles %10 | %11"}
                         .arg(framesPerSecond, 0, 'f', 1)
                         .arg(Simulation_.CurrentTick().Value())
                         .arg(PendingSimulationTicks_.Value())
                         .arg(navigation)
                         .arg(visibleTiles)
                         .arg(Tiles_.size())
                         .arg(Dirty_.size())
                         .arg(Jobs_.size())
                         .arg(drawCalls)
                         .arg(triangles)
                         .arg(Status_));
    FrameCount_ = 0;
    FrameClock_.restart();
}

} // namespace UnrealVoxelSim::Testbed::Qt
