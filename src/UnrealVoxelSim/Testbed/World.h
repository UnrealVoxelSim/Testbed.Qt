#pragma once

#include "UnrealVoxelSim/Ecs/Api/EntityId.h"
#include "UnrealVoxelSim/Movement/Api/ProfileId.h"
#include "UnrealVoxelSim/Navigation/Api/ExecutionStateComponent.h"
#include "UnrealVoxelSim/Profiling/Api/IRecorder.h"
#include "UnrealVoxelSim/Simulation/Api/IPacer.h"
#include "UnrealVoxelSim/Simulation/Api/IStepper.h"
#include "UnrealVoxelSim/Spatial/Api/LinearVelocity.h"
#include "UnrealVoxelSim/Spatial/Api/Position.h"
#include "UnrealVoxelSim/Voxel/Api/IBounds.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/IChangeSource.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/IReader.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/IRegionReader.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/MaterialId.h"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace UnrealVoxelSim::Testbed
{
	struct WorldDescriptor final
	{
		std::string_view Id;
		std::string_view DisplayName;
		std::size_t InitialPopulation{};
		bool AutonomousNavigation{};
	};

	struct RuntimeStats final
	{
		Simulation::Api::TickIndex Tick;
		std::size_t PawnCount{};
		std::array<std::size_t, 6> NavigationCounts{};
	};

	struct PawnState final
	{
		Spatial::Api::Position Location;
		Spatial::Api::LinearVelocity Velocity;
		Movement::Api::ProfileId Profile;
		bool Grounded{};
	};

	class World final
	{
	public:
		~World();
		World(const World&) = delete;
		World& operator=(const World&) = delete;
		World(World&&) noexcept;
		World& operator=(World&&) noexcept;

		[[nodiscard]] const WorldDescriptor& Descriptor() const noexcept;
		[[nodiscard]] const Voxel::Api::IBounds& Bounds() const noexcept;
		[[nodiscard]] const Voxel::Solid::Api::IReader& Solids() const noexcept;
		[[nodiscard]] const Voxel::Solid::Api::IRegionReader& SolidRegions() const noexcept;
		[[nodiscard]] Voxel::Solid::Api::IChangeSource& SolidChanges() noexcept;
		[[nodiscard]] Simulation::Api::IPacer& Pacer() noexcept;
		[[nodiscard]] Simulation::Api::IStepper& Stepper() noexcept;

		[[nodiscard]] std::span<const Ecs::Api::EntityId> Pawns() const noexcept;
		[[nodiscard]] std::optional<PawnState> ReadPawn(Ecs::Api::EntityId entity) const noexcept;
		[[nodiscard]] std::optional<Navigation::Api::ExecutionStateComponent>
		ReadNavigation(Ecs::Api::EntityId entity) const noexcept;
		[[nodiscard]] std::size_t TargetPopulation() const noexcept;
		[[nodiscard]] std::size_t MaximumPopulation() const noexcept;
		void SetTargetPopulation(std::size_t population);

		[[nodiscard]] bool Fill(Voxel::Api::Region region, Voxel::Solid::Api::MaterialId material);
		[[nodiscard]] bool Erase(Voxel::Api::Region region);
		[[nodiscard]] bool Navigate(Ecs::Api::EntityId pawn, Voxel::Api::Position destination);
		[[nodiscard]] RuntimeStats Stats() const noexcept;

	private:
		class Impl;
		explicit World(std::unique_ptr<Impl> implementation) noexcept;
		std::unique_ptr<Impl> m_Impl;

		friend class WorldCatalog;
	};

	class WorldCatalog final
	{
	public:
		[[nodiscard]] static std::span<const WorldDescriptor> Worlds() noexcept;
		[[nodiscard]] static std::unique_ptr<World> Create(std::string_view id, Profiling::Api::IRecorder& profiling);
	};
} // namespace UnrealVoxelSim::Testbed
