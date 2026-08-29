#pragma once

#include "UnrealVoxelSim/Ecs/Api/EntityId.h"
#include "UnrealVoxelSim/Ecs/EnTT/Registry.h"
#include "UnrealVoxelSim/Movement/Api/ProfileComponent.h"
#include "UnrealVoxelSim/Movement/Api/ProfileId.h"
#include "UnrealVoxelSim/Navigation/Api/ExecutionId.h"
#include "UnrealVoxelSim/Navigation/Api/ICommandSink.h"
#include "UnrealVoxelSim/Profiling/Api/IRecorder.h"
#include "UnrealVoxelSim/Simulation/Api/IDecisionUpdater.h"
#include "UnrealVoxelSim/Simulation/Api/TickCount.h"
#include "UnrealVoxelSim/Spatial/Api/Position.h"
#include "UnrealVoxelSim/Voxel/Api/Region.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace UnrealVoxelSim::Testbed
{
	class PawnController final : public Simulation::Api::IDecisionUpdater
	{
	public:
		struct Configuration final
		{
			Movement::Api::ProfileId Profile;
			Voxel::Api::Region DestinationBounds;
			Voxel::Api::Region SpawnRegion;
			std::int32_t SpawnZ{};
			std::int32_t SurfaceDestinationZ{};
			std::int32_t SpawnSpacing{1};
			std::size_t InitialPopulation{1};
			bool AutonomousNavigation{};
			Simulation::Api::TickCount RetryDelay{60};
			std::uint64_t RandomSeed{0xA0761D6478BD642FULL};
		};

		PawnController(Ecs::EnTT::Registry& entities,
					   Navigation::Api::ICommandSink& navigationCommands,
					   Profiling::Api::IRecorder& profiling,
					   Configuration configuration);

		void SetTargetPopulation(std::size_t population);
		void TrackExternalNavigation(Ecs::Api::EntityId entity) noexcept;
		void UpdateDecisions(Simulation::Api::StepContext context) override;

		[[nodiscard]] std::span<const Ecs::Api::EntityId> Entities() const noexcept;
		[[nodiscard]] std::size_t TargetPopulation() const noexcept;
		[[nodiscard]] std::size_t MaximumPopulation() const noexcept;
		[[nodiscard]] bool IsAutonomous() const noexcept;

	private:
		struct State final
		{
			std::uint64_t RandomState{};
			std::uint64_t RetryTick{};
			bool ActiveExecution{};
			bool ExternalCommandPending{};
		};

		struct PendingCancel final
		{
			Ecs::Api::EntityId Entity;
			Navigation::Api::ExecutionId Execution;
		};

		[[nodiscard]] std::uint64_t NextRandom(State& state) const noexcept;
		[[nodiscard]] std::int32_t
		RandomCoordinate(State& state, std::int32_t minimum, std::int32_t maximum) const noexcept;
		[[nodiscard]] Spatial::Api::Position Spawn(std::size_t index) const noexcept;
		[[nodiscard]] Spatial::Api::Position Destination(State& state) const noexcept;
		void CreatePawn(std::size_t index);
		void RemovePawn();

		Ecs::EnTT::Registry& m_EntitiesRegistry;
		Navigation::Api::ICommandSink& m_NavigationCommands;
		Profiling::Api::IRecorder& m_Profiling;
		Configuration m_Configuration;

		// TODO Use ECS components
		std::vector<Ecs::Api::EntityId> m_Entities;

		// TODO States and Cancels should be stored in ECS as components.
		std::vector<State> m_States;
		std::vector<PendingCancel> m_PendingCancels;
		std::size_t m_TargetPopulation{};
		std::size_t m_MaximumPopulation{};
		std::uint64_t m_CommandSequence{};
		std::uint64_t m_ExecutionSequence{};
	};
} // namespace UnrealVoxelSim::Testbed
