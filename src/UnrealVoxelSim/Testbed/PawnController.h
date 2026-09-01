#pragma once

#include "UnrealVoxelSim/Ecs/Api/EntityId.h"
#include "UnrealVoxelSim/Ecs/EnTT/Registry.h"
#include "UnrealVoxelSim/Movement/Api/ProfileComponent.h"
#include "UnrealVoxelSim/Movement/Api/ProfileId.h"
#include "UnrealVoxelSim/Movement/Api/IIntentReceiver.h"
#include "UnrealVoxelSim/Navigation/Api/INavigation.h"
#include "UnrealVoxelSim/Profiling/Api/IRecorder.h"
#include "UnrealVoxelSim/Simulation/Api/IStepParticipant.h"
#include "UnrealVoxelSim/Simulation/Api/TickCount.h"
#include "UnrealVoxelSim/Spatial/Api/Position.h"
#include "UnrealVoxelSim/Voxel/Api/Region.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace UnrealVoxelSim::Testbed
{
	class PawnController final : public Simulation::Api::IStepParticipant
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
					   Navigation::Api::INavigation& navigation,
					   Movement::Api::IIntentReceiver& movementIntent,
					   Profiling::Api::IRecorder& profiling,
					   Configuration configuration);

		void SetTargetPopulation(std::size_t population);
		void TrackExternalNavigation(Ecs::Api::EntityId entity) noexcept;
		void Step(Simulation::Api::StepContext context) override;

		[[nodiscard]] std::vector<Ecs::Api::EntityId> Entities() const;
		[[nodiscard]] std::size_t TargetPopulation() const noexcept;
		[[nodiscard]] std::size_t MaximumPopulation() const noexcept;
		[[nodiscard]] bool IsAutonomous() const noexcept;

	private:
		struct PawnComponent final
		{
			std::size_t Slot{};
		};

		struct PawnStateComponent final
		{
			std::uint64_t RandomState{};
			std::uint64_t RetryTick{};
			bool ActiveExecution{};
			bool ExternalCommandPending{};
		};

		using ActivePawnQuery = Ecs::Api::Query<Ecs::Api::Read<PawnComponent>>;

		[[nodiscard]] std::vector<Ecs::Api::EntityId> ActivePawns() const;
		[[nodiscard]] std::uint64_t NextRandom(PawnStateComponent& state) const noexcept;
		[[nodiscard]] std::int32_t
		RandomCoordinate(PawnStateComponent& state, std::int32_t minimum, std::int32_t maximum) const noexcept;
		[[nodiscard]] Spatial::Api::Position Spawn(std::size_t index) const noexcept;
		[[nodiscard]] Spatial::Api::Position Destination(PawnStateComponent& state) const noexcept;
		void CreatePawn(std::size_t index);
		void RemovePawn(Ecs::Api::EntityId entity);

		Ecs::EnTT::Registry& m_EntitiesRegistry;
		Navigation::Api::INavigation& m_Navigation;
		Movement::Api::IIntentReceiver& m_MovementIntent;
		Profiling::Api::IRecorder& m_Profiling;
		Configuration m_Configuration;

		std::size_t m_TargetPopulation{};
		std::size_t m_MaximumPopulation{};
	};
} // namespace UnrealVoxelSim::Testbed
