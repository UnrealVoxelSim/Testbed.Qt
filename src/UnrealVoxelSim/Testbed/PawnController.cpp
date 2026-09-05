#include "PawnController.h"

#include "UnrealVoxelSim/Math/Api/FixedPointScalar.h"
#include "UnrealVoxelSim/Movement/Api/GroundedComponent.h"
#include "UnrealVoxelSim/Navigation/Api/ExecutionState.h"
#include "UnrealVoxelSim/Navigation/Api/Goal.h"
#include "UnrealVoxelSim/Navigation/Api/ExecutionStateComponent.h"
#include "UnrealVoxelSim/Profiling/Api/Macros.h"
#include "UnrealVoxelSim/Spatial/Api/LinearVelocityComponent.h"
#include "UnrealVoxelSim/Spatial/Api/PositionComponent.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace UnrealVoxelSim::Testbed
{
	namespace
	{
		[[nodiscard]] constexpr std::uint64_t SaturatingAdd(const std::uint64_t left,
															const std::uint64_t right) noexcept
		{
			return left > std::numeric_limits<std::uint64_t>::max() - right ? std::numeric_limits<std::uint64_t>::max()
																			: left + right;
		}
	} // namespace

	PawnController::PawnController(Ecs::EnTT::Registry& entities,
	                               Navigation::Api::INavigation& navigation,
	                               Movement::Api::IIntentReceiver& movementIntent,
								   Profiling::Api::IRecorder& profiling,
								   Configuration configuration) :
		m_EntitiesRegistry(entities), m_Navigation(navigation), m_MovementIntent(movementIntent), m_Profiling(profiling),
		m_Configuration(configuration)
	{
		if (!m_Configuration.Profile.IsValid() || !m_Configuration.DestinationBounds.IsValid() ||
			m_Configuration.DestinationBounds.IsEmpty() || !m_Configuration.SpawnRegion.IsValid() ||
			m_Configuration.SpawnRegion.IsEmpty() || m_Configuration.SpawnSpacing <= 0)
			throw std::invalid_argument{"Pawn controller configuration is invalid."};

		const auto width =
			static_cast<std::uint64_t>(m_Configuration.SpawnRegion.Max.X - m_Configuration.SpawnRegion.Min.X);
		const auto length =
			static_cast<std::uint64_t>(m_Configuration.SpawnRegion.Max.Y - m_Configuration.SpawnRegion.Min.Y);
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
		for (std::size_t index = m_ActivePawns.size(); index < m_TargetPopulation; ++index)
			CreatePawn(index);
		while (m_ActivePawns.size() > m_TargetPopulation)
		{
			RemovePawn(m_ActivePawns.back());
			m_ActivePawns.pop_back();
		}
	}

	void PawnController::TrackExternalNavigation(const Ecs::Api::EntityId entity) noexcept
	{
		if (!m_EntitiesRegistry.IsAlive(entity) || !m_EntitiesRegistry.Contains<PawnComponent>(entity))
			return;
		const auto state = m_EntitiesRegistry.Get<PawnStateComponent>(entity);
		if (!state)
			return;
		state->get().ActiveExecution = true;
		state->get().ExternalCommandPending = true;
	}

	void PawnController::Step(const Simulation::Api::StepContext context)
	{
		UNREALVOXELSIM_PROFILE_ZONE(m_Profiling, "Pawn controller update");
		const auto& pawns = ActivePawns();
		std::size_t started{};
		if (m_Configuration.AutonomousNavigation)
		{
			for (const auto entity : pawns)
			{
				const auto stateResult = m_EntitiesRegistry.Get<PawnStateComponent>(entity);
				if (!stateResult)
					continue;
				auto& state = stateResult->get();
				if (state.ActiveExecution)
				{
					if (state.ExternalCommandPending)
					{
						state.ExternalCommandPending = false;
					}
					else
					{
						const auto execution =
							m_EntitiesRegistry.Get<Navigation::Api::ExecutionStateComponent>(entity);
						if (!execution || execution->get().State == Navigation::Api::ExecutionState::Arrived ||
							execution->get().State == Navigation::Api::ExecutionState::Unreachable ||
							execution->get().State == Navigation::Api::ExecutionState::Cancelled)
						{
							state.ActiveExecution = false;
							state.RetryTick = SaturatingAdd(context.Tick.Value(), m_Configuration.RetryDelay.Value());
						}
					}
				}
				if (state.ActiveExecution || context.Tick.Value() < state.RetryTick)
					continue;

				constexpr auto arrivalRadius = Math::Api::FixedPointScalar::OneRaw / 4;
				if (m_Navigation.BeginNavigateToGoal(
						entity, {Destination(state), Math::Api::FixedPointScalar::FromRaw(arrivalRadius)}))
				{
					state.ActiveExecution = true;
					++started;
				}
			}
		}

		UNREALVOXELSIM_PROFILE_PLOT(m_Profiling, "Pawn population", pawns.size());
		UNREALVOXELSIM_PROFILE_PLOT(m_Profiling, "Pawn navigation starts", started);
		m_NavigationStarts += started;
	}

	std::vector<Ecs::Api::EntityId> PawnController::Entities() const { return m_ActivePawns; }

	const std::vector<Ecs::Api::EntityId>& PawnController::ActivePawns() const noexcept
	{
		return m_ActivePawns;
	}

	std::size_t PawnController::TargetPopulation() const noexcept { return m_TargetPopulation; }

	std::size_t PawnController::MaximumPopulation() const noexcept { return m_MaximumPopulation; }

	bool PawnController::IsAutonomous() const noexcept { return m_Configuration.AutonomousNavigation; }

	std::size_t PawnController::NavigationStarts() const noexcept { return m_NavigationStarts; }

	std::uint64_t PawnController::NextRandom(PawnStateComponent& state) const noexcept
	{
		auto value = state.RandomState;
		value ^= value >> 12U;
		value ^= value << 25U;
		value ^= value >> 27U;
		state.RandomState = value;
		return value * 0x2545F4914F6CDD1DULL;
	}

	std::int32_t PawnController::RandomCoordinate(PawnStateComponent& state,
												  const std::int32_t minimum,
												  const std::int32_t maximum) const noexcept
	{
		const auto width = static_cast<std::uint64_t>(static_cast<std::int64_t>(maximum) - minimum);
		return static_cast<std::int32_t>(static_cast<std::int64_t>(minimum) +
										 static_cast<std::int64_t>(NextRandom(state) % width));
	}

	Spatial::Api::Position PawnController::Spawn(const std::size_t index) const noexcept
	{
		const auto width =
			static_cast<std::uint64_t>(m_Configuration.SpawnRegion.Max.X - m_Configuration.SpawnRegion.Min.X);
		const auto columns = (width + static_cast<std::uint64_t>(m_Configuration.SpawnSpacing) - 1) /
			static_cast<std::uint64_t>(m_Configuration.SpawnSpacing);
		const auto length =
			static_cast<std::uint64_t>(m_Configuration.SpawnRegion.Max.Y - m_Configuration.SpawnRegion.Min.Y);
		const auto rows = (length + static_cast<std::uint64_t>(m_Configuration.SpawnSpacing) - 1) /
			static_cast<std::uint64_t>(m_Configuration.SpawnSpacing);
		const auto slots = columns * rows;
		const auto slot = static_cast<std::uint64_t>(index) * 104729ULL % slots;
		const auto x = static_cast<std::int64_t>(m_Configuration.SpawnRegion.Min.X) +
			static_cast<std::int64_t>(slot % columns) * m_Configuration.SpawnSpacing;
		const auto y = static_cast<std::int64_t>(m_Configuration.SpawnRegion.Min.Y) +
			static_cast<std::int64_t>(slot / columns) * m_Configuration.SpawnSpacing;
		constexpr auto half = Math::Api::FixedPointScalar::OneRaw / 2;
		return {Math::Api::FixedPointScalar::FromRaw(x * Math::Api::FixedPointScalar::OneRaw + half),
				Math::Api::FixedPointScalar::FromRaw(y * Math::Api::FixedPointScalar::OneRaw + half),
				Math::Api::FixedPointScalar::FromWhole(m_Configuration.SpawnZ)};
	}

	Spatial::Api::Position PawnController::Destination(PawnStateComponent& state) const noexcept
	{
		const auto x =
			RandomCoordinate(state, m_Configuration.DestinationBounds.Min.X, m_Configuration.DestinationBounds.Max.X);
		const auto y =
			RandomCoordinate(state, m_Configuration.DestinationBounds.Min.Y, m_Configuration.DestinationBounds.Max.Y);
		const auto surface = NextRandom(state) % 2U == 0;
		const auto z = surface
			? m_Configuration.SurfaceDestinationZ
			: RandomCoordinate(state, m_Configuration.DestinationBounds.Min.Z, m_Configuration.DestinationBounds.Max.Z);
		constexpr auto half = Math::Api::FixedPointScalar::OneRaw / 2;
		return {Math::Api::FixedPointScalar::FromRaw(
					static_cast<std::int64_t>(x) * Math::Api::FixedPointScalar::OneRaw + half),
				Math::Api::FixedPointScalar::FromRaw(
					static_cast<std::int64_t>(y) * Math::Api::FixedPointScalar::OneRaw + half),
				Math::Api::FixedPointScalar::FromWhole(z)};
	}

	void PawnController::CreatePawn(const std::size_t index)
	{
		const auto entity = m_EntitiesRegistry.Create();
		auto seed = m_Configuration.RandomSeed + static_cast<std::uint64_t>(index) * 0x9E3779B97F4A7C15ULL;
		seed ^= seed >> 30U;
		seed *= 0xBF58476D1CE4E5B9ULL;
		seed ^= seed >> 27U;
		seed *= 0x94D049BB133111EBULL;
		seed ^= seed >> 31U;
		if (seed == 0)
			seed = 1;
		if (!m_EntitiesRegistry.Assign<PawnComponent>(entity, PawnComponent{index}) ||
			!m_EntitiesRegistry.Assign<PawnStateComponent>(entity, PawnStateComponent{seed}) ||
			!m_EntitiesRegistry.Assign<Spatial::Api::PositionComponent>(entity, Spawn(index)) ||
			!m_EntitiesRegistry.Assign<Spatial::Api::LinearVelocityComponent>(entity, Spatial::Api::LinearVelocity{}) ||
			!m_EntitiesRegistry.Assign<Movement::Api::ProfileComponent>(entity, m_Configuration.Profile) ||
			!m_EntitiesRegistry.Assign<Movement::Api::GroundedComponent>(entity, true))
		{
			static_cast<void>(m_EntitiesRegistry.Destroy(entity));
			throw std::runtime_error{"A navigation pawn could not be registered with movement."};
		}
		if (!m_MovementIntent.SetIntent(entity, {}, {}))
		{
			static_cast<void>(m_EntitiesRegistry.Destroy(entity));
			throw std::runtime_error{"A navigation pawn could not receive neutral movement intent."};
		}
		m_ActivePawns.push_back(entity);
	}

	void PawnController::RemovePawn(const Ecs::Api::EntityId entity)
	{
		if (!m_EntitiesRegistry.IsAlive(entity) || !m_EntitiesRegistry.Contains<PawnComponent>(entity))
			throw std::logic_error{"Pawn population state became inconsistent."};
		m_Navigation.CancelNavigateToGoal(entity);
		if (!m_EntitiesRegistry.Destroy(entity))
			throw std::logic_error{"Pawn population state became inconsistent."};
	}
} // namespace UnrealVoxelSim::Testbed
