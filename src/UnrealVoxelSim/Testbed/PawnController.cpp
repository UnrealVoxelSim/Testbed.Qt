#include "PawnController.h"

#include "UnrealVoxelSim/Movement/Api/Position.h"
#include "UnrealVoxelSim/Movement/Api/Scalar.h"
#include "UnrealVoxelSim/Navigation/Api/Cancel.h"
#include "UnrealVoxelSim/Navigation/Api/Command.h"
#include "UnrealVoxelSim/Navigation/Api/ExecutionState.h"
#include "UnrealVoxelSim/Navigation/Api/Goal.h"
#include "UnrealVoxelSim/Navigation/Api/Start.h"
#include "UnrealVoxelSim/Profiling/Api/Macros.h"
#include "UnrealVoxelSim/Simulation/Api/CommandSourceId.h"
#include "UnrealVoxelSim/Simulation/Api/CommandStamp.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace UnrealVoxelSim::Testbed
{
namespace
{
constexpr Simulation::Api::CommandSourceId AutonomousNavigationSource{3};

[[nodiscard]] constexpr std::uint64_t SaturatingAdd(const std::uint64_t left,
                                                    const std::uint64_t right) noexcept
{
    return left > std::numeric_limits<std::uint64_t>::max() - right
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}
} // namespace

PawnController::PawnController(Ecs::EnTT::Registry &entities, Movement::Api::ICommands &movement,
                               Navigation::Api::ICommandSink &navigationCommands,
                               const Navigation::Api::IExecutionReader &navigationExecutions,
                               Profiling::Api::IRecorder &profiling, Configuration configuration)
    : m_EntitiesRegistry(entities), m_Movement(movement), m_NavigationCommands(navigationCommands),
      m_NavigationExecutions(navigationExecutions), m_Profiling(profiling), m_Configuration(configuration)
{
    if (!m_Configuration.Profile.IsValid() || !m_Configuration.DestinationBounds.IsValid() ||
        m_Configuration.DestinationBounds.IsEmpty() || !m_Configuration.SpawnRegion.IsValid() ||
        m_Configuration.SpawnRegion.IsEmpty() || m_Configuration.SpawnSpacing <= 0)
        throw std::invalid_argument{"Pawn controller configuration is invalid."};

    const auto width = static_cast<std::uint64_t>(m_Configuration.SpawnRegion.Max.X -
                                                  m_Configuration.SpawnRegion.Min.X);
    const auto length = static_cast<std::uint64_t>(m_Configuration.SpawnRegion.Max.Y -
                                                   m_Configuration.SpawnRegion.Min.Y);
    const auto columns = (width + static_cast<std::uint64_t>(m_Configuration.SpawnSpacing) - 1) /
                         static_cast<std::uint64_t>(m_Configuration.SpawnSpacing);
    const auto rows = (length + static_cast<std::uint64_t>(m_Configuration.SpawnSpacing) - 1) /
                      static_cast<std::uint64_t>(m_Configuration.SpawnSpacing);
    if (columns == 0 || rows == 0 || columns > std::numeric_limits<std::size_t>::max() / rows)
        throw std::invalid_argument{"Pawn spawn region cannot provide a bounded population."};
    m_MaximumPopulation = static_cast<std::size_t>(columns * rows);
    SetTargetPopulation(m_Configuration.InitialPopulation);
}

void PawnController::SetTargetPopulation(const std::size_t population)
{
    UNREALVOXELSIM_PROFILE_ZONE(m_Profiling, "Pawn population resize");
    m_TargetPopulation = std::min(population, m_MaximumPopulation);
    while (m_Entities.size() < m_TargetPopulation) CreatePawn(m_Entities.size());
    while (m_Entities.size() > m_TargetPopulation) RemovePawn();
}

void PawnController::TrackExternalNavigation(const Ecs::Api::EntityId entity) noexcept
{
    const auto iterator = std::ranges::find(m_Entities, entity);
    if (iterator == m_Entities.end()) return;
    auto &state = m_States[static_cast<std::size_t>(std::distance(m_Entities.begin(), iterator))];
    state.ActiveExecution = true;
    state.ExternalCommandPending = true;
}

void PawnController::UpdateDecisions(const Simulation::Api::StepContext context)
{
    UNREALVOXELSIM_PROFILE_ZONE(m_Profiling, "Pawn controller update");
    std::vector<Navigation::Api::Command> commands;
    commands.reserve(m_PendingCancels.size() + m_Entities.size());
    for (const auto pending : m_PendingCancels)
        commands.emplace_back(Navigation::Api::Cancel{
            {context.Tick, AutonomousNavigationSource, ++m_CommandSequence}, pending.Entity, pending.Execution});

    std::vector<std::size_t> started;
    if (m_Configuration.AutonomousNavigation)
    {
        started.reserve(m_Entities.size());
        for (std::size_t index = 0; index < m_Entities.size(); ++index)
        {
            auto &state = m_States[index];
            if (state.ActiveExecution)
            {
                if (state.ExternalCommandPending)
                {
                    state.ExternalCommandPending = false;
                }
                else
                {
                    const auto execution = m_NavigationExecutions.ReadExecution(m_Entities[index]);
                    if (!execution || execution->State == Navigation::Api::ExecutionState::Arrived ||
                        execution->State == Navigation::Api::ExecutionState::Unreachable ||
                        execution->State == Navigation::Api::ExecutionState::Cancelled)
                    {
                        state.ActiveExecution = false;
                        state.RetryTick = SaturatingAdd(context.Tick.Value(), m_Configuration.RetryDelay.Value());
                    }
                }
            }
            if (state.ActiveExecution || context.Tick.Value() < state.RetryTick) continue;

            constexpr auto arrivalRadius = Movement::Api::Scalar::OneRaw / 4;
            commands.emplace_back(Navigation::Api::Start{
                {context.Tick, AutonomousNavigationSource, ++m_CommandSequence}, m_Entities[index],
                Navigation::Api::ExecutionId{++m_ExecutionSequence},
                {Destination(state), Movement::Api::Scalar::FromRaw(arrivalRadius)}});
            started.push_back(index);
        }
    }

    UNREALVOXELSIM_PROFILE_PLOT(m_Profiling, "Pawn population", m_Entities.size());
    UNREALVOXELSIM_PROFILE_PLOT(m_Profiling, "Pawn navigation commands", commands.size());
    if (commands.empty()) return;
    if (m_NavigationCommands.Submit(commands))
    {
        m_PendingCancels.clear();
        for (const auto index : started) m_States[index].ActiveExecution = true;
    }
}

std::span<const Ecs::Api::EntityId> PawnController::Entities() const noexcept
{
    return m_Entities;
}

std::size_t PawnController::TargetPopulation() const noexcept
{
    return m_TargetPopulation;
}

std::size_t PawnController::MaximumPopulation() const noexcept
{
    return m_MaximumPopulation;
}

bool PawnController::IsAutonomous() const noexcept
{
    return m_Configuration.AutonomousNavigation;
}

std::uint64_t PawnController::NextRandom(State &state) const noexcept
{
    auto value = state.RandomState;
    value ^= value >> 12U;
    value ^= value << 25U;
    value ^= value >> 27U;
    state.RandomState = value;
    return value * 0x2545F4914F6CDD1DULL;
}

std::int32_t PawnController::RandomCoordinate(State &state, const std::int32_t minimum,
                                              const std::int32_t maximum) const noexcept
{
    const auto width = static_cast<std::uint64_t>(static_cast<std::int64_t>(maximum) - minimum);
    return static_cast<std::int32_t>(static_cast<std::int64_t>(minimum) +
                                     static_cast<std::int64_t>(NextRandom(state) % width));
}

Movement::Api::Position PawnController::Spawn(const std::size_t index) const noexcept
{
    const auto width = static_cast<std::uint64_t>(m_Configuration.SpawnRegion.Max.X -
                                                  m_Configuration.SpawnRegion.Min.X);
    const auto columns = (width + static_cast<std::uint64_t>(m_Configuration.SpawnSpacing) - 1) /
                         static_cast<std::uint64_t>(m_Configuration.SpawnSpacing);
    const auto length = static_cast<std::uint64_t>(m_Configuration.SpawnRegion.Max.Y -
                                                   m_Configuration.SpawnRegion.Min.Y);
    const auto rows = (length + static_cast<std::uint64_t>(m_Configuration.SpawnSpacing) - 1) /
                      static_cast<std::uint64_t>(m_Configuration.SpawnSpacing);
    const auto slots = columns * rows;
    const auto slot = static_cast<std::uint64_t>(index) * 104729ULL % slots;
    const auto x = static_cast<std::int64_t>(m_Configuration.SpawnRegion.Min.X) +
                   static_cast<std::int64_t>(slot % columns) * m_Configuration.SpawnSpacing;
    const auto y = static_cast<std::int64_t>(m_Configuration.SpawnRegion.Min.Y) +
                   static_cast<std::int64_t>(slot / columns) * m_Configuration.SpawnSpacing;
    constexpr auto half = Movement::Api::Scalar::OneRaw / 2;
    return {Movement::Api::Scalar::FromRaw(x * Movement::Api::Scalar::OneRaw + half),
            Movement::Api::Scalar::FromRaw(y * Movement::Api::Scalar::OneRaw + half),
            Movement::Api::Scalar::FromWhole(m_Configuration.SpawnZ)};
}

Movement::Api::Position PawnController::Destination(State &state) const noexcept
{
    const auto x = RandomCoordinate(state, m_Configuration.DestinationBounds.Min.X,
                                    m_Configuration.DestinationBounds.Max.X);
    const auto y = RandomCoordinate(state, m_Configuration.DestinationBounds.Min.Y,
                                    m_Configuration.DestinationBounds.Max.Y);
    const auto surface = NextRandom(state) % 2U == 0;
    const auto z = surface ? m_Configuration.SurfaceDestinationZ
                           : RandomCoordinate(state, m_Configuration.DestinationBounds.Min.Z,
                                              m_Configuration.DestinationBounds.Max.Z);
    constexpr auto half = Movement::Api::Scalar::OneRaw / 2;
    return {Movement::Api::Scalar::FromRaw(static_cast<std::int64_t>(x) * Movement::Api::Scalar::OneRaw + half),
            Movement::Api::Scalar::FromRaw(static_cast<std::int64_t>(y) * Movement::Api::Scalar::OneRaw + half),
            Movement::Api::Scalar::FromWhole(z)};
}

void PawnController::CreatePawn(const std::size_t index)
{
    const auto entity = m_EntitiesRegistry.Create();
    if (!m_Movement.Add({entity, m_Configuration.Profile, Spawn(index)}))
    {
        static_cast<void>(m_EntitiesRegistry.Destroy(entity));
        throw std::runtime_error{"A navigation pawn could not be registered with movement."};
    }
    auto seed = m_Configuration.RandomSeed + static_cast<std::uint64_t>(index) * 0x9E3779B97F4A7C15ULL;
    seed ^= seed >> 30U;
    seed *= 0xBF58476D1CE4E5B9ULL;
    seed ^= seed >> 27U;
    seed *= 0x94D049BB133111EBULL;
    seed ^= seed >> 31U;
    if (seed == 0) seed = 1;
    m_Entities.push_back(entity);
    m_States.push_back({seed});
}

void PawnController::RemovePawn()
{
    const auto entity = m_Entities.back();
    if (const auto execution = m_NavigationExecutions.ReadExecution(entity))
        m_PendingCancels.push_back({entity, execution->Execution});
    if (!m_Movement.Remove(entity) || !m_EntitiesRegistry.Destroy(entity))
        throw std::logic_error{"Pawn population state became inconsistent."};
    m_Entities.pop_back();
    m_States.pop_back();
}

} // namespace UnrealVoxelSim::Testbed
