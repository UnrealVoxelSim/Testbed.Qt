#include "World.h"

#include "PawnController.h"

#include "UnrealVoxelSim/Composition/Game.h"
#include "UnrealVoxelSim/Ecs/EnTT/Registry.h"
#include "UnrealVoxelSim/Math/Api/FixedPointScalar.h"
#include "UnrealVoxelSim/Movement/Api/GroundedComponent.h"
#include "UnrealVoxelSim/Movement/Api/GroundedProfile.h"
#include "UnrealVoxelSim/Movement/Api/ProfileComponent.h"
#include "UnrealVoxelSim/Navigation/Api/ExecutionState.h"
#include "UnrealVoxelSim/Simulation/Api/IStepParticipant.h"
#include "UnrealVoxelSim/Spatial/Api/LinearVelocityComponent.h"
#include "UnrealVoxelSim/Spatial/Api/PositionComponent.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/Cell.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/Placement.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/StandardMaterials.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace UnrealVoxelSim::Testbed
{
	namespace
	{
		enum class TerrainKind
		{
			Hills,
			Flat,
		};

		struct WorldConfiguration final
		{
			WorldDescriptor Descriptor;
			Voxel::Api::Region Bounds;
			Voxel::Api::Region InitialNavigationRegion;
			Voxel::Api::Region SpawnRegion;
			std::int32_t SpawnZ{};
			std::int32_t SpawnSpacing{1};
			TerrainKind Terrain{};
		};

		constexpr std::array Configurations{
			WorldConfiguration{{"standard", "Standard World", 1, false},
							   {{-256, -256, -64}, {256, 256, 128}},
							   {{-96, -96, -8}, {96, 96, 7}},
							   {{0, 0, 4}, {1, 1, 5}},
							   4,
							   1,
							   TerrainKind::Hills},
			WorldConfiguration{{"stress", "GetNavigation Stress", 1'000, true},
							   {{-512, -512, -64}, {512, 512, 192}},
							   {{-192, -192, 0}, {192, 192, 3}},
							   {{-192, -192, 1}, {192, 192, 2}},
							   1,
							   2,
							   TerrainKind::Flat},
		};

		[[nodiscard]] constexpr auto BuildWorldDescriptors() noexcept
		{
			std::array<WorldDescriptor, Configurations.size()> descriptors{};
			for (std::size_t index = 0; index < Configurations.size(); ++index)
				descriptors[index] = Configurations[index].Descriptor;
			return descriptors;
		}

		constexpr auto WorldDescriptors = BuildWorldDescriptors();
		constexpr std::size_t FineExpansionsPerTick = 64;
		constexpr std::size_t ComponentExpansionsPerTick = 4;

		[[nodiscard]] const WorldConfiguration* FindConfiguration(const std::string_view id) noexcept
		{
			const auto result =
				std::ranges::find(Configurations,
								  id,
								  [](const WorldConfiguration& configuration) { return configuration.Descriptor.Id; });
			return result == Configurations.end() ? nullptr : &*result;
		}

		[[nodiscard]] std::vector<Voxel::Solid::Api::Placement> BuildTerrain(const WorldConfiguration& configuration)
		{
			namespace Materials = Voxel::Solid::Api::StandardMaterials;
			std::vector<Voxel::Solid::Api::Placement> terrain;
			if (configuration.Terrain == TerrainKind::Flat)
			{
				const auto width = static_cast<std::size_t>(configuration.Bounds.Max.X - configuration.Bounds.Min.X);
				const auto length = static_cast<std::size_t>(configuration.Bounds.Max.Y - configuration.Bounds.Min.Y);
				terrain.reserve(width * length);
				for (std::int32_t y = configuration.Bounds.Min.Y; y < configuration.Bounds.Max.Y; ++y)
					for (std::int32_t x = configuration.Bounds.Min.X; x < configuration.Bounds.Max.X; ++x)
						terrain.push_back({{x, y, 0}, Materials::Grass});
				return terrain;
			}

			terrain.reserve(400'000);
			for (std::int32_t y = -96; y < 96; ++y)
				for (std::int32_t x = -96; x < 96; ++x)
				{
					const auto height =
						static_cast<std::int32_t>(std::round(std::sin(static_cast<double>(x) * 0.055) * 3.0 +
															 std::cos(static_cast<double>(y) * 0.047) * 3.0));
					for (std::int32_t z = -8; z <= height; ++z)
					{
						auto material = Materials::Stone;
						if (z == height)
							material = Materials::Grass;
						else if (z >= height - 2)
							material = Materials::Dirt;
						terrain.push_back({{x, y, z}, material});
					}
				}
			return terrain;
		}
	} // namespace

	World::World(const std::string_view id, Profiling::Api::IRecorder& profiling)
	{
		const auto* configuration = FindConfiguration(id);
		if (configuration == nullptr)
			throw std::invalid_argument{"Unknown test world: " + std::string{id}};
		m_Descriptor = configuration->Descriptor;

		constexpr std::array materials{Voxel::Solid::Api::StandardMaterials::Dirt,
									   Voxel::Solid::Api::StandardMaterials::Grass,
									   Voxel::Solid::Api::StandardMaterials::Stone};
		const std::array profiles{Movement::Api::GroundedProfile{Movement::Api::ProfileId{1}}};
		const std::array navigationPreparation{configuration->InitialNavigationRegion};
		m_Game = std::make_unique<Composition::Game>(
			Composition::GameConfiguration{
				.Bounds = configuration->Bounds,
				.RegistryScope = Ecs::Api::RegistryScopeId{1},
				.PlannerExpansionsPerTick = FineExpansionsPerTick,
				.PlannerMaximumExpansionsPerRequest = 65'536,
				.ReachabilityExpansionsPerTick = ComponentExpansionsPerTick,
				.NavigationTileBuildsPerStep = 16,
				.NavigationComponentCellsPerTick = 256,
			},
			materials,
			profiles,
			navigationPreparation,
			profiling);

		auto terrain = BuildTerrain(*configuration);
		if (!m_Game->GetSolidVoxelPlacer().Place(terrain))
			throw std::runtime_error{"The initial solid terrain could not be created."};

		m_Pawns = std::make_unique<PawnController>(
			m_Game->GetEcsRegistry(),
			m_Game->GetNavigation(),
			m_Game->GetMovementIntentReceiver(),
			profiling,
			PawnController::Configuration{profiles[0].Id,
										  configuration->Bounds,
										  configuration->SpawnRegion,
										  configuration->SpawnZ,
										  1,
										  configuration->SpawnSpacing,
										  configuration->Descriptor.InitialPopulation,
										  configuration->Descriptor.AutonomousNavigation});
		const std::array<Simulation::Api::IStepParticipant*, 1> testbedParticipants{m_Pawns.get()};
		m_Game->SetPreDomainParticipants(testbedParticipants);
	}

	World::~World() = default;
	World::World(World&&) noexcept = default;
	World& World::operator=(World&&) noexcept = default;

	const WorldDescriptor& World::Descriptor() const noexcept { return m_Descriptor; }
	const Voxel::Api::IBounds& World::Bounds() const noexcept { return m_Game->GetVoxelWorldBoundsProvider(); }
	const Voxel::Solid::Api::IReader& World::Solids() const noexcept { return m_Game->GetSolidVoxelsReader(); }
	const Voxel::Solid::Api::IRegionReader& World::SolidRegions() const noexcept { return m_Game->GetSolidVoxelsRegionReader(); }
	Voxel::Solid::Api::IChangeSource& World::SolidChanges() noexcept { return m_Game->GetSolidVoxelsChangeSource(); }
	Voxel::Solid::Api::IPlacer& World::SolidPlacement() noexcept { return m_Game->GetSolidVoxelPlacer(); }
	Voxel::Solid::Api::IRemover& World::SolidRemoval() noexcept { return m_Game->GetSolidVoxelRemover(); }
	Navigation::Api::INavigation& World::Navigation() noexcept { return m_Game->GetNavigation(); }
	Simulation::Api::IPacer& World::Pacer() noexcept { return m_Game->GetSimulationPacer(); }
	Simulation::Api::IStepper& World::Stepper() noexcept { return m_Game->GetSimulationStepper(); }
	std::vector<Ecs::Api::EntityId> World::Pawns() const { return m_Pawns->Entities(); }

	std::optional<PawnState> World::ReadPawn(const Ecs::Api::EntityId entity) const noexcept
	{
		const auto& entities = m_Game->GetEcsRegistry();
		const auto position = entities.Get<Spatial::Api::PositionComponent>(entity);
		const auto velocity = entities.Get<Spatial::Api::LinearVelocityComponent>(entity);
		const auto profile = entities.Get<Movement::Api::ProfileComponent>(entity);
		const auto grounded = entities.Get<Movement::Api::GroundedComponent>(entity);
		if (!position || !velocity || !profile || !grounded)
			return std::nullopt;
		return PawnState{position->get().Value, velocity->get().Value, profile->get().Profile, grounded->get().Value};
	}

	std::optional<Navigation::Api::ExecutionStateComponent>
	World::ReadNavigation(const Ecs::Api::EntityId entity) const noexcept
	{
		const auto execution = m_Game->GetEcsRegistry().Get<Navigation::Api::ExecutionStateComponent>(entity);
		return execution ? std::optional{execution->get()} : std::nullopt;
	}

	std::size_t World::TargetPopulation() const noexcept { return m_Pawns->TargetPopulation(); }
	std::size_t World::MaximumPopulation() const noexcept { return m_Pawns->MaximumPopulation(); }
	void World::SetTargetPopulation(const std::size_t population) { m_Pawns->SetTargetPopulation(population); }

	bool World::Fill(const Voxel::Api::Region region, const Voxel::Solid::Api::MaterialId material)
	{
		if (!region.IsValid() || region.IsEmpty() || !material.IsValid())
			return false;
		std::vector<Voxel::Solid::Api::Cell> cells(*region.CellCount());
		if (!m_Game->GetSolidVoxelsRegionReader().ReadRegion(region, cells))
			return false;
		std::vector<Voxel::Solid::Api::Placement> placements;
		placements.reserve(cells.size());
		std::size_t index{};
		for (auto z = region.Min.Z; z < region.Max.Z; ++z)
			for (auto y = region.Min.Y; y < region.Max.Y; ++y)
				for (auto x = region.Min.X; x < region.Max.X; ++x)
					if (cells[index++].IsEmpty())
						placements.push_back({{x, y, z}, material});
		return placements.empty() || m_Game->GetSolidVoxelPlacer().Place(placements).has_value();
	}

	bool World::Erase(const Voxel::Api::Region region)
	{
		if (!region.IsValid() || region.IsEmpty())
			return false;
		std::vector<Voxel::Solid::Api::Cell> cells(*region.CellCount());
		if (!m_Game->GetSolidVoxelsRegionReader().ReadRegion(region, cells))
			return false;
		std::vector<Voxel::Api::Position> positions;
		positions.reserve(cells.size());
		std::size_t index{};
		for (auto z = region.Min.Z; z < region.Max.Z; ++z)
			for (auto y = region.Min.Y; y < region.Max.Y; ++y)
				for (auto x = region.Min.X; x < region.Max.X; ++x)
					if (!cells[index++].IsEmpty())
						positions.push_back({x, y, z});
		return positions.empty() || m_Game->GetSolidVoxelRemover().Remove(positions).has_value();
	}

	bool World::Navigate(const Ecs::Api::EntityId pawn, const Voxel::Api::Position destination)
	{
		const auto pawns = m_Pawns->Entities();
		if (std::ranges::find(pawns, pawn) == pawns.end())
			return false;
		constexpr auto one = Math::Api::FixedPointScalar::OneRaw;
		constexpr auto half = one / 2;
		const Spatial::Api::Position goal{
			Math::Api::FixedPointScalar::FromRaw(static_cast<std::int64_t>(destination.X) * one + half),
			Math::Api::FixedPointScalar::FromRaw(static_cast<std::int64_t>(destination.Y) * one + half),
			Math::Api::FixedPointScalar::FromRaw(static_cast<std::int64_t>(destination.Z) * one)};
		const auto started =
			m_Game->GetNavigation().BeginNavigateToGoal(pawn, {goal, Math::Api::FixedPointScalar::FromRaw(one / 4)}).has_value();
		if (started)
			m_Pawns->TrackExternalNavigation(pawn);
		return started;
	}

	RuntimeStats World::Stats() const noexcept
	{
		RuntimeStats result{m_Game->GetSimulationStepper().CurrentTick(), m_Pawns->Entities().size()};
		const auto& entities = m_Game->GetEcsRegistry();
		for (const auto pawn : m_Pawns->Entities())
		{
			const auto execution = entities.Get<Navigation::Api::ExecutionStateComponent>(pawn);
			if (!execution)
				continue;
			switch (execution->get().State)
			{
			case Navigation::Api::ExecutionState::Planning:
				++result.NavigationCounts[0];
				break;
			case Navigation::Api::ExecutionState::Following:
				++result.NavigationCounts[1];
				break;
			case Navigation::Api::ExecutionState::Replanning:
				++result.NavigationCounts[2];
				break;
			case Navigation::Api::ExecutionState::Arrived:
				++result.NavigationCounts[3];
				break;
			case Navigation::Api::ExecutionState::Unreachable:
				++result.NavigationCounts[4];
				break;
			case Navigation::Api::ExecutionState::Cancelled:
				++result.NavigationCounts[5];
				break;
			}
		}
		return result;
	}

	std::span<const WorldDescriptor> WorldCatalog::Worlds() noexcept { return WorldDescriptors; }

	std::unique_ptr<World> WorldCatalog::Create(const std::string_view id, Profiling::Api::IRecorder& profiling)
	{
		if (FindConfiguration(id) == nullptr)
			return nullptr;
		return std::unique_ptr<World>{new World{id, profiling}};
	}
}
