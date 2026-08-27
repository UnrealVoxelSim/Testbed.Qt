#include "Viewport.h"

#include "UnrealVoxelSim/Profiling/Api/Macros.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/Changed.h"
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
layout(location = 3) in vec3 instanceOffset;

uniform mat4 viewProjection;
uniform vec3 modelOffset;

out vec3 normal;
flat out uint surfaceId;

void main()
{
    gl_Position = viewProjection * vec4(position + modelOffset + instanceOffset, 1.0);
    normal = vertexNormal;
    surfaceId = surface;
}
)";

constexpr auto FragmentShader = R"(
#version 330 core
in vec3 normal;
flat in uint surfaceId;
out vec4 color;
uniform bool highlighted;

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
    vec3 baseColor = highlighted ? vec3(0.10, 0.95, 1.0) : surfaceColor(surfaceId);
    color = vec4(baseColor * illumination, 1.0);
}
)";

}

Viewport::Viewport(UnrealVoxelSim::Testbed::World &world,
                   UnrealVoxelSim::Profiling::Api::IRecorder &profiling, QWidget *parent)
    : QOpenGLWidget(parent), m_World(world), m_Profiling(profiling),
      m_Sampler(world.Bounds(), world.SolidRegions())
{
    setFocusPolicy(::Qt::StrongFocus);
    setMouseTracking(true);
    m_MovementClock.start();
    m_FrameClock.start();
    m_ChangesSubscription =
        world.SolidChanges().Subscribe([this](const UnrealVoxelSim::Voxel::Solid::Api::Changed &changed) noexcept {
            for (const auto region : changed.Regions)
            {
                MarkDirty(region);
            }
        });
}

Viewport::~Viewport()
{
    m_ChangesSubscription.Reset();
    for (auto &job : m_Jobs)
    {
        job.Future.wait();
    }
    if (isValid())
    {
        makeCurrent();
        for (auto &[key, tile] : m_Tiles)
        {
            static_cast<void>(key);
            DestroyGpu(tile);
        }
        DestroyGpu(m_PawnGpu);
        if (m_PawnInstanceBuffer != 0) glDeleteBuffers(1, &m_PawnInstanceBuffer);
        m_Program.reset();
        doneCurrent();
    }
}

void Viewport::SetTool(const Tool tool) noexcept
{
    m_Tool = tool;
    m_LastBrushCell.reset();
}

void Viewport::SetBrushSize(const int size) noexcept
{
    m_BrushSize = std::clamp(size, 1, 9);
    m_LastBrushCell.reset();
}

void Viewport::SetMaterial(const UnrealVoxelSim::Voxel::Solid::Api::MaterialId material) noexcept
{
    if (material.IsValid())
    {
        m_Material = material;
    }
}

void Viewport::SetRenderDistance(const int distance) noexcept
{
    m_RenderDistance = std::clamp(distance, MinimumRenderDistance, MaximumRenderDistance);
}

void Viewport::SetDiagnosticsSink(std::function<void(const QString &)> sink)
{
    m_DiagnosticsSink = std::move(sink);
}

void Viewport::SetSimulationBacklog(const UnrealVoxelSim::Simulation::Api::TickCount pending) noexcept
{
    m_PendingSimulationTicks = pending;
}

void Viewport::ReportStatus(QString status)
{
    m_Status = std::move(status);
}

void Viewport::Tick()
{
    UNREALVOXELSIM_PROFILE_ZONE(m_Profiling, "Viewport update request");
    if (m_SelectedPawn && !m_World.ReadPawn(*m_SelectedPawn))
    {
        m_SelectedPawn.reset();
        m_Status = "Selected pawn no longer exists";
    }
    const auto elapsedNanoseconds = m_MovementClock.nsecsElapsed();
    m_MovementClock.restart();
    const auto elapsed = std::min(static_cast<float>(elapsedNanoseconds) / 1'000'000'000.0F, 0.1F);
    const auto speed = m_Keys.contains(::Qt::Key_Shift) ? 45.0F : 18.0F;
    const auto forward = Forward();
    auto right = QVector3D::crossProduct(forward, QVector3D{0.0F, 0.0F, 1.0F});
    if (!right.isNull())
    {
        right.normalize();
    }
    QVector3D movement;
    if (m_Keys.contains(::Qt::Key_W))
        movement += forward;
    if (m_Keys.contains(::Qt::Key_S))
        movement -= forward;
    if (m_Keys.contains(::Qt::Key_D))
        movement += right;
    if (m_Keys.contains(::Qt::Key_A))
        movement -= right;
    if (m_Keys.contains(::Qt::Key_E))
        movement += QVector3D{0.0F, 0.0F, 1.0F};
    if (m_Keys.contains(::Qt::Key_Q))
        movement -= QVector3D{0.0F, 0.0F, 1.0F};
    if (!movement.isNull())
    {
        m_Camera += movement.normalized() * speed * elapsed;
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

    m_Program = std::make_unique<QOpenGLShaderProgram>();
    if (!m_Program->addShaderFromSourceCode(QOpenGLShader::Vertex, VertexShader) ||
        !m_Program->addShaderFromSourceCode(QOpenGLShader::Fragment, FragmentShader) || !m_Program->link())
    {
        m_Status = QString{"Shader initialization failed: %1"}.arg(m_Program->log());
    }
    else
    {
        CreatePawnGpu();
    }
}

void Viewport::paintGL()
{
    UNREALVOXELSIM_PROFILE_ZONE(m_Profiling, "Qt render frame");
    {
        UNREALVOXELSIM_PROFILE_ZONE(m_Profiling, "Terrain residency");
        EnsureResidency();
    }
    {
        UNREALVOXELSIM_PROFILE_ZONE(m_Profiling, "Terrain job completion");
        ProcessJobs();
    }
    {
        UNREALVOXELSIM_PROFILE_ZONE(m_Profiling, "Terrain job scheduling");
        ScheduleJobs();
    }

    glClearColor(0.52F, 0.72F, 0.92F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!m_Program || !m_Program->isLinked())
    {
        return;
    }

    const auto viewProjection = ViewProjection();
    m_Program->bind();
    m_Program->setUniformValue("viewProjection", viewProjection);
    m_Program->setUniformValue("modelOffset", QVector3D{});
    m_Program->setUniformValue("highlighted", false);
    std::size_t visibleTiles{};
    std::size_t drawCalls{};
    std::size_t triangles{};
    {
        UNREALVOXELSIM_PROFILE_ZONE(m_Profiling, "Terrain draw submission");
        for (const auto &[key, tile] : m_Tiles)
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
    }
    std::vector<GpuOffset> pawnOffsets;
    pawnOffsets.reserve(m_World.Pawns().size());
    std::optional<GpuOffset> selectedPawnOffset;
    const auto maximumPawnDistanceSquared = static_cast<double>(m_RenderDistance) * m_RenderDistance;
    {
        UNREALVOXELSIM_PROFILE_ZONE(m_Profiling, "Pawn visibility collection");
        for (const auto entity : m_World.Pawns())
        {
            const auto pawn = m_World.ReadPawn(entity);
            if (!pawn) continue;
            const auto x = pawn->Location.X.ToDouble();
            const auto y = pawn->Location.Y.ToDouble();
            const auto z = pawn->Location.Z.ToDouble();
            const auto dx = x - m_Camera.x();
            const auto dy = y - m_Camera.y();
            const auto dz = z - m_Camera.z();
            if (dx * dx + dy * dy + dz * dz > maximumPawnDistanceSquared) continue;
            const auto clip = viewProjection * QVector4D{static_cast<float>(x), static_cast<float>(y),
                                                         static_cast<float>(z + 0.9), 1.0F};
            if (clip.w() <= 0.0F || clip.x() < -clip.w() || clip.x() > clip.w() || clip.y() < -clip.w() ||
                clip.y() > clip.w() || clip.z() < -clip.w() || clip.z() > clip.w())
                continue;
            const GpuOffset offset{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
            if (m_SelectedPawn == entity)
                selectedPawnOffset = offset;
            else
                pawnOffsets.push_back(offset);
        }
    }
    if (!pawnOffsets.empty() && m_PawnGpu.IndexCount != 0)
    {
        glBindVertexArray(m_PawnGpu.VertexArray);
        glBindBuffer(GL_ARRAY_BUFFER, m_PawnInstanceBuffer);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(pawnOffsets.size() * sizeof(GpuOffset)),
                     pawnOffsets.data(), GL_DYNAMIC_DRAW);
        glDrawElementsInstanced(GL_TRIANGLES, m_PawnGpu.IndexCount, GL_UNSIGNED_INT, nullptr,
                                static_cast<GLsizei>(pawnOffsets.size()));
        ++drawCalls;
        triangles += static_cast<std::size_t>(m_PawnGpu.IndexCount) / 3 * pawnOffsets.size();
    }
    if (selectedPawnOffset && m_PawnGpu.IndexCount != 0)
    {
        m_Program->setUniformValue("highlighted", true);
        glBindVertexArray(m_PawnGpu.VertexArray);
        glBindBuffer(GL_ARRAY_BUFFER, m_PawnInstanceBuffer);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(GpuOffset)), &*selectedPawnOffset,
                     GL_DYNAMIC_DRAW);
        glDrawElementsInstanced(GL_TRIANGLES, m_PawnGpu.IndexCount, GL_UNSIGNED_INT, nullptr, 1);
        m_Program->setUniformValue("highlighted", false);
        ++drawCalls;
        triangles += static_cast<std::size_t>(m_PawnGpu.IndexCount) / 3;
    }
    glBindVertexArray(0);
    m_Program->release();
    UNREALVOXELSIM_PROFILE_PLOT(m_Profiling, "Resident terrain tiles", m_Tiles.size());
    UNREALVOXELSIM_PROFILE_PLOT(m_Profiling, "Dirty terrain tiles", m_Dirty.size());
    UNREALVOXELSIM_PROFILE_PLOT(m_Profiling, "Terrain mesh jobs", m_Jobs.size());
    const auto visiblePawns = pawnOffsets.size() + static_cast<std::size_t>(selectedPawnOffset.has_value());
    UNREALVOXELSIM_PROFILE_PLOT(m_Profiling, "Visible pawns", visiblePawns);
    UNREALVOXELSIM_PROFILE_PLOT(m_Profiling, "Render draw calls", drawCalls);
    PublishDiagnostics(visibleTiles, visiblePawns, drawCalls, triangles);
    UNREALVOXELSIM_PROFILE_FRAME(m_Profiling, "Presentation");
}

void Viewport::resizeGL(const int width, const int height)
{
    glViewport(0, 0, width, height);
}

void Viewport::keyPressEvent(QKeyEvent *event)
{
    if (!event->isAutoRepeat())
    {
        m_Keys.insert(event->key());
    }
    QOpenGLWidget::keyPressEvent(event);
}

void Viewport::keyReleaseEvent(QKeyEvent *event)
{
    if (!event->isAutoRepeat())
    {
        m_Keys.erase(event->key());
    }
    QOpenGLWidget::keyReleaseEvent(event);
}

void Viewport::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    m_LastMouse = event->position().toPoint();
    if (event->button() == ::Qt::RightButton)
    {
        m_Looking = true;
    }
    if (event->button() == ::Qt::LeftButton)
    {
        m_Painting = true;
        m_LastBrushCell.reset();
        ApplyBrush(event->position().toPoint());
    }
}

void Viewport::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == ::Qt::RightButton)
    {
        m_Looking = false;
    }
    if (event->button() == ::Qt::LeftButton)
    {
        m_Painting = false;
        m_LastBrushCell.reset();
    }
}

void Viewport::mouseMoveEvent(QMouseEvent *event)
{
    const auto position = event->position().toPoint();
    if (m_Looking)
    {
        const auto delta = position - m_LastMouse;
        m_Yaw -= static_cast<float>(delta.x()) * 0.18F;
        m_Pitch = std::clamp(m_Pitch - static_cast<float>(delta.y()) * 0.18F, -89.0F, 89.0F);
    }
    if (m_Painting)
    {
        ApplyBrush(position);
    }
    m_LastMouse = position;
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
    const auto bounds = m_World.Bounds().Bounds();
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
    const auto yaw = m_Yaw * DegreesToRadians;
    const auto pitch = m_Pitch * DegreesToRadians;
    return QVector3D{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)}.normalized();
}

QMatrix4x4 Viewport::ViewProjection() const
{
    QMatrix4x4 projection;
    projection.perspective(65.0F, static_cast<float>(std::max(width(), 1)) / static_cast<float>(std::max(height(), 1)),
                           0.1F, static_cast<float>(m_RenderDistance + TileEdge * 2));
    QMatrix4x4 view;
    view.lookAt(m_Camera, m_Camera + Forward(), QVector3D{0.0F, 0.0F, 1.0F});
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
    const auto renderRadius = (m_RenderDistance + TileEdge - 1) / TileEdge;
    const TileKey center{FloorDiv(static_cast<std::int32_t>(std::floor(m_Camera.x()))),
                         FloorDiv(static_cast<std::int32_t>(std::floor(m_Camera.y()))),
                         FloorDiv(static_cast<std::int32_t>(std::floor(m_Camera.z())))};
    if (m_ResidencyCenter == center && m_ResidencyRadius == renderRadius)
    {
        return;
    }
    m_ResidencyCenter = center;
    m_ResidencyRadius = renderRadius;

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
                const auto [iterator, inserted] = m_Tiles.try_emplace(key);
                if (inserted)
                {
                    iterator->second.Queued = true;
                    m_Dirty.push_back(key);
                }
            }
        }
    }

    for (auto iterator = m_Tiles.begin(); iterator != m_Tiles.end();)
    {
        if (!desired.contains(iterator->first))
        {
            DestroyGpu(iterator->second);
            iterator = m_Tiles.erase(iterator);
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
    const auto bounds = m_World.Bounds().Bounds();
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
                const auto iterator = m_Tiles.find(key);
                if (iterator == m_Tiles.end())
                {
                    continue;
                }
                ++iterator->second.Generation;
                if (!iterator->second.Queued)
                {
                    iterator->second.Queued = true;
                    m_Dirty.push_back(key);
                }
            }
        }
    }
}

void Viewport::ProcessJobs()
{
    for (auto iterator = m_Jobs.begin(); iterator != m_Jobs.end();)
    {
        if (iterator->Future.wait_for(std::chrono::seconds{0}) != std::future_status::ready)
        {
            ++iterator;
            continue;
        }
        auto result = iterator->Future.get();
        const auto tile = m_Tiles.find(iterator->Key);
        if (tile != m_Tiles.end() && tile->second.Generation == iterator->Generation)
        {
            if (result)
            {
                Upload(tile->second, *result);
                tile->second.UploadedGeneration = iterator->Generation;
            }
            else
            {
                m_Status = "Meshing failed";
            }
        }
        iterator = m_Jobs.erase(iterator);
    }
}

void Viewport::ScheduleJobs()
{
    const auto hardwareThreads = std::max(std::thread::hardware_concurrency(), 2U);
    const auto maximumJobs = static_cast<std::size_t>(std::min(hardwareThreads - 1, 4U));
    std::size_t captured{};
    while (!m_Dirty.empty() && m_Jobs.size() < maximumJobs && captured < 2)
    {
        const auto key = m_Dirty.front();
        m_Dirty.pop_front();
        const auto tile = m_Tiles.find(key);
        if (tile == m_Tiles.end())
        {
            continue;
        }
        tile->second.Queued = false;
        const auto generation = tile->second.Generation;
        auto snapshot = m_Sampler.Capture(TileRegion(key));
        if (!snapshot)
        {
            m_Status = "Voxel snapshot capture failed";
            tile->second.UploadedGeneration = generation;
            continue;
        }
        m_Jobs.push_back(
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

std::optional<Viewport::Ray> Viewport::ScreenRay(const QPoint &screenPosition) const
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
    return Ray{origin, (farPoint.toVector3D() - origin).normalized()};
}

std::optional<Viewport::Hit> Viewport::Raycast(const QPoint &screenPosition) const
{
    const auto ray = ScreenRay(screenPosition);
    if (!ray) return std::nullopt;
    const auto &origin = ray->Origin;
    const auto &direction = ray->Direction;

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
    const auto bounds = m_World.Bounds().Bounds();
    for (std::size_t iteration = 0; iteration < 768 && distance <= 256.0F; ++iteration)
    {
        const UnrealVoxelSim::Voxel::Api::Position position{cell[0], cell[1], cell[2]};
        if (bounds.Contains(position))
        {
            const auto value = m_World.Solids().Read(position);
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
    if (m_Tool == Tool::Select)
    {
        SelectPawn(screenPosition);
        return;
    }
    if (m_Tool == Tool::Navigate)
    {
        SubmitNavigation(screenPosition);
        return;
    }
    const auto hit = Raycast(screenPosition);
    if (!hit)
    {
        m_Status = "No solid voxel under cursor";
        return;
    }
    auto center = hit->Cell;
    if (m_Tool == Tool::Fill)
    {
        center.X += hit->Normal.X;
        center.Y += hit->Normal.Y;
        center.Z += hit->Normal.Z;
    }
    if (m_LastBrushCell == center)
    {
        return;
    }
    m_LastBrushCell = center;

    const auto bounds = m_World.Bounds().Bounds();
    const auto before = (m_BrushSize - 1) / 2;
    const auto after = m_BrushSize / 2;
    const UnrealVoxelSim::Voxel::Api::Region region{
        {std::max(bounds.Min.X, center.X - before), std::max(bounds.Min.Y, center.Y - before),
         std::max(bounds.Min.Z, center.Z - before)},
        {std::min(bounds.Max.X, center.X + after + 1), std::min(bounds.Max.Y, center.Y + after + 1),
         std::min(bounds.Max.Z, center.Z + after + 1)}};
    if (!region.IsValid() || region.IsEmpty())
    {
        m_Status = "Brush is outside world bounds";
        return;
    }

    if (m_Tool == Tool::Erase)
    {
        m_Status = m_World.Erase(region) ? "Erase command queued" : "Erase command rejected";
    }
    else
    {
        m_Status = m_World.Fill(region, m_Material) ? "Fill command queued" : "Fill command rejected";
    }
}

void Viewport::SelectPawn(const QPoint &screenPosition)
{
    const auto ray = ScreenRay(screenPosition);
    if (!ray)
    {
        m_Status = "Pawn selection ray is unavailable";
        return;
    }
    std::optional<UnrealVoxelSim::Ecs::Api::EntityId> selected;
    auto nearest = std::numeric_limits<float>::max();
    for (const auto entity : m_World.Pawns())
    {
        const auto state = m_World.ReadPawn(entity);
        if (!state) continue;
        const QVector3D center{static_cast<float>(state->Location.X.ToDouble()),
                               static_cast<float>(state->Location.Y.ToDouble()),
                               static_cast<float>(state->Location.Z.ToDouble() + 0.9)};
        const auto offset = center - ray->Origin;
        const auto distance = QVector3D::dotProduct(offset, ray->Direction);
        if (distance < 0.0F || distance > 256.0F || distance >= nearest) continue;
        const auto closest = ray->Origin + ray->Direction * distance;
        if ((center - closest).lengthSquared() > 0.9F * 0.9F) continue;
        nearest = distance;
        selected = entity;
    }
    m_SelectedPawn = selected;
    m_Status = selected ? "Pawn selected" : "No pawn under cursor";
}

void Viewport::SubmitNavigation(const QPoint &screenPosition)
{
    if (!m_SelectedPawn)
    {
        m_Status = "Select a pawn before issuing navigation";
        return;
    }
    const auto hit = Raycast(screenPosition);
    if (!hit)
    {
        m_Status = "No navigation destination under cursor";
        return;
    }
    auto foot = hit->Cell;
    foot.X += hit->Normal.X;
    foot.Y += hit->Normal.Y;
    foot.Z += hit->Normal.Z;
    m_Status = m_World.Navigate(*m_SelectedPawn, foot) ? "Navigation execution queued"
                                                    : "Navigation command rejected";
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
    glGenVertexArrays(1, &m_PawnGpu.VertexArray);
    glGenBuffers(1, &m_PawnGpu.VertexBuffer);
    glGenBuffers(1, &m_PawnGpu.IndexBuffer);
    glBindVertexArray(m_PawnGpu.VertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, m_PawnGpu.VertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(GpuVertex)), vertices.data(),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_PawnGpu.IndexBuffer);
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
    glGenBuffers(1, &m_PawnInstanceBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, m_PawnInstanceBuffer);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(GpuOffset), nullptr);
    glVertexAttribDivisor(3, 1);
    glBindVertexArray(0);
    m_PawnGpu.IndexCount = static_cast<int>(indices.size());
}

void Viewport::PublishDiagnostics(const std::size_t visibleTiles, const std::size_t visiblePawns,
                                  const std::size_t drawCalls, const std::size_t triangles)
{
    ++m_FrameCount;
    const auto elapsed = m_FrameClock.elapsed();
    if (elapsed < 500 || !m_DiagnosticsSink)
    {
        return;
    }
    const auto framesPerSecond = static_cast<double>(m_FrameCount) * 1000.0 / static_cast<double>(elapsed);
    const auto runtime = m_World.Stats();
    const auto &navigationCounts = runtime.NavigationCounts;
    const auto navigation = QString{"P%1 F%2 R%3 A%4 U%5 C%6"}
                                .arg(navigationCounts[0])
                                .arg(navigationCounts[1])
                                .arg(navigationCounts[2])
                                .arg(navigationCounts[3])
                                .arg(navigationCounts[4])
                                .arg(navigationCounts[5]);
    m_DiagnosticsSink(QString{"%1 FPS | tick %2 | lag %3 | pawns %4/%5 | nav %6 | tiles %7/%8 | rebuild %9/%10 | draws %11 | triangles %12 | %13"}
                         .arg(framesPerSecond, 0, 'f', 1)
                         .arg(runtime.Tick.Value())
                         .arg(m_PendingSimulationTicks.Value())
                         .arg(visiblePawns)
                         .arg(runtime.PawnCount)
                         .arg(navigation)
                         .arg(visibleTiles)
                         .arg(m_Tiles.size())
                         .arg(m_Dirty.size())
                         .arg(m_Jobs.size())
                         .arg(drawCalls)
                         .arg(triangles)
                         .arg(m_Status));
    m_FrameCount = 0;
    m_FrameClock.restart();
}

}
