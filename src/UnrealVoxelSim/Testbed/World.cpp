#include "World.h"

#include "PawnController.h"

#include "UnrealVoxelSim/Ecs/Api/RegistryScopeId.h"
#include "UnrealVoxelSim/Ecs/EnTT/Registry.h"
#include "UnrealVoxelSim/Events/Api/IPublisher.h"
#include "UnrealVoxelSim/Events/InMemory/Dispatcher.h"
#include "UnrealVoxelSim/Math/Api/FixedPointScalar.h"
#include "UnrealVoxelSim/Movement/Api/GroundedComponent.h"
#include "UnrealVoxelSim/Movement/Api/GroundedProfile.h"
#include "UnrealVoxelSim/Movement/Api/MovementProfileComponent.h"
#include "UnrealVoxelSim/Movement/Voxel/Controller.h"
#include "UnrealVoxelSim/Navigation/Api/Command.h"
#include "UnrealVoxelSim/Navigation/Api/ExecutionState.h"
#include "UnrealVoxelSim/Navigation/Api/Goal.h"
#include "UnrealVoxelSim/Navigation/Api/NavigationExecutionComponent.h"
#include "UnrealVoxelSim/Navigation/Api/Start.h"
#include "UnrealVoxelSim/Navigation/Following/Controller.h"
#include "UnrealVoxelSim/Navigation/Voxel/Planner.h"
#include "UnrealVoxelSim/Simulation/Api/CommandSourceId.h"
#include "UnrealVoxelSim/Simulation/Api/CommandStamp.h"
#include "UnrealVoxelSim/Simulation/FixedStep/Controller.h"
#include "UnrealVoxelSim/Simulation/Pipeline/Pipeline.h"
#include "UnrealVoxelSim/Spatial/Api/LinearVelocityComponent.h"
#include "UnrealVoxelSim/Spatial/Api/PositionComponent.h"
#include "UnrealVoxelSim/Voxel/Chunked/Field.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/Cell.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/Changed.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/EraseCommand.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/FillCommand.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/Placement.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/QueuedCommand.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/StandardMaterials.h"
#include "UnrealVoxelSim/Voxel/Solid/Commands/Queue.h"
#include "UnrealVoxelSim/Voxel/Solid/Controller.h"
#include "UnrealVoxelSim/Voxel/Solid/Navigation/Environment.h"
#include "UnrealVoxelSim/Voxel/Solid/Navigation/InvalidationBridge.h"

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
			WorldConfiguration{{"stress", "Navigation Stress", 1'000, true},
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

		constexpr Simulation::Api::CommandSourceId ManualNavigationSource{1};
		constexpr Simulation::Api::CommandSourceId SolidToolSource{2};
		constexpr std::size_t FineExpansionsPerTick = 64;
		constexpr std::size_t ComponentExpansionsPerTick = 4;

		using EntityRegistry = Ecs::EnTT::Registry;
		using MovementController = Movement::Voxel::Controller;
		using FollowingController = Navigation::Following::Controller;

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
				for (auto y = configuration.Bounds.Min.Y; y < configuration.Bounds.Max.Y; ++y)
					for (auto x = configuration.Bounds.Min.X; x < configuration.Bounds.Max.X; ++x)
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

	class World::Impl final
	{
	public:
		Impl(const WorldConfiguration& configuration, Profiling::Api::IRecorder& profiling) :
			Descriptor(configuration.Descriptor), Profiling(profiling)
		{
			Dispatcher = std::make_unique<Events::InMemory::Dispatcher>();
			Field = std::make_unique<Voxel::Chunked::Field>(configuration.Bounds);
			constexpr std::array materials{Voxel::Solid::Api::StandardMaterials::Dirt,
										   Voxel::Solid::Api::StandardMaterials::Grass,
										   Voxel::Solid::Api::StandardMaterials::Stone};
			auto solidChanges = Dispatcher->CreateChannel<Voxel::Solid::Api::Changed>();
			auto& solidChangePublisher =
				static_cast<Events::Api::IPublisher<Voxel::Solid::Api::Changed>&>(*solidChanges);
			Solids = std::make_unique<Voxel::Solid::Controller>(
				*Field, *Field, *Field, materials, std::move(solidChanges), solidChangePublisher);
			auto terrain = BuildTerrain(configuration);
			if (!Solids->Place(terrain))
				throw std::runtime_error{"The initial solid terrain could not be created."};

			Entities = std::make_unique<Ecs::EnTT::Registry>(Ecs::Api::RegistryScopeId{1});
			Movement =
				std::make_unique<MovementController>(MovementController::Access{*Entities}, *Solids, MovementProfiles);
			NavigationEnvironment = std::make_unique<Voxel::Solid::Navigation::Environment>(*Field, *Solids);
			SolidCommands = std::make_unique<Voxel::Solid::Commands::Queue>(*Solids);
			Planner = std::make_unique<Navigation::Voxel::Planner>(
				*NavigationEnvironment,
				MovementProfiles,
				Profiling,
				FineExpansionsPerTick,
				Navigation::Voxel::Planner::DefaultMaximumExpansionsPerRequest,
				ComponentExpansionsPerTick,
				Navigation::Voxel::Planner::DefaultTileBuildsPerTopologyUpdate);
			const std::array initialNavigationRegions{configuration.InitialNavigationRegion};
			Planner->Prepare(initialNavigationRegions);
			Following = std::make_unique<FollowingController>(
				FollowingController::Access{*Entities}, *Planner, MovementProfiles);
			Pawns = std::make_unique<PawnController>(
				*Entities,
				*Following,
				Profiling,
				PawnController::Configuration{MovementProfiles[0].Id,
											  configuration.Bounds,
											  configuration.SpawnRegion,
											  configuration.SpawnZ,
											  1,
											  configuration.SpawnSpacing,
											  configuration.Descriptor.InitialPopulation,
											  configuration.Descriptor.AutonomousNavigation});
			NavigationInvalidation =
				std::make_unique<Voxel::Solid::Navigation::InvalidationBridge>(Solids->Changes(), *Planner);
			Pipeline = std::make_unique<Simulation::Pipeline::Pipeline>(
				*SolidCommands, *Dispatcher, *Pawns, *Following, *Planner, *Planner, *Following, *Movement, Profiling);
			Simulation = std::make_unique<Simulation::FixedStep::Controller>(*Pipeline);
		}

		WorldDescriptor Descriptor;
		Profiling::Api::IRecorder& Profiling;
		std::array<Movement::Api::GroundedProfile, 1> MovementProfiles{
			Movement::Api::GroundedProfile{Movement::Api::ProfileId{1}}};
		std::unique_ptr<Events::InMemory::Dispatcher> Dispatcher;
		std::unique_ptr<Voxel::Chunked::Field> Field;
		std::unique_ptr<Voxel::Solid::Controller> Solids;
		std::unique_ptr<EntityRegistry> Entities;
		std::unique_ptr<MovementController> Movement;
		std::unique_ptr<Voxel::Solid::Navigation::Environment> NavigationEnvironment;
		std::unique_ptr<Voxel::Solid::Commands::Queue> SolidCommands;
		std::unique_ptr<Navigation::Voxel::Planner> Planner;
		std::unique_ptr<FollowingController> Following;
		std::unique_ptr<PawnController> Pawns;
		std::unique_ptr<Voxel::Solid::Navigation::InvalidationBridge> NavigationInvalidation;
		std::unique_ptr<Simulation::Pipeline::Pipeline> Pipeline;
		std::unique_ptr<Simulation::FixedStep::Controller> Simulation;
		std::uint64_t SolidSequence{};
		std::uint64_t NavigationSequence{};
		std::uint64_t NavigationExecution{1ULL << 63U};
	};

	World::World(std::unique_ptr<Impl> implementation) noexcept : m_Impl(std::move(implementation)) {}

	World::~World() = default;
	World::World(World&&) noexcept = default;
	World& World::operator=(World&&) noexcept = default;

	const WorldDescriptor& World::Descriptor() const noexcept { return m_Impl->Descriptor; }

	const Voxel::Api::IBounds& World::Bounds() const noexcept { return *m_Impl->Field; }

	const Voxel::Solid::Api::IReader& World::Solids() const noexcept { return *m_Impl->Solids; }

	const Voxel::Solid::Api::IRegionReader& World::SolidRegions() const noexcept { return *m_Impl->Solids; }

	Voxel::Solid::Api::IChangeSource& World::SolidChanges() noexcept { return m_Impl->Solids->Changes(); }

	Simulation::Api::IPacer& World::Pacer() noexcept { return *m_Impl->Simulation; }

	Simulation::Api::IStepper& World::Stepper() noexcept { return *m_Impl->Simulation; }

	std::span<const Ecs::Api::EntityId> World::Pawns() const noexcept { return m_Impl->Pawns->Entities(); }

	std::optional<PawnState> World::ReadPawn(const Ecs::Api::EntityId entity) const noexcept
	{
		const auto position = m_Impl->Entities->Get<Spatial::Api::PositionComponent>(entity);
		const auto velocity = m_Impl->Entities->Get<Spatial::Api::LinearVelocityComponent>(entity);
		const auto profile = m_Impl->Entities->Get<Movement::Api::MovementProfileComponent>(entity);
		const auto grounded = m_Impl->Entities->Get<Movement::Api::GroundedComponent>(entity);
		if (!position || !velocity || !profile || !grounded)
			return std::nullopt;
		return PawnState{position->get().Value, velocity->get().Value, profile->get().Profile, grounded->get().Value};
	}

	std::optional<Navigation::Api::NavigationExecutionComponent>
	World::ReadNavigation(const Ecs::Api::EntityId entity) const noexcept
	{
		const auto execution = m_Impl->Entities->Get<Navigation::Api::NavigationExecutionComponent>(entity);
		return execution ? std::optional{execution->get()} : std::nullopt;
	}

	std::size_t World::TargetPopulation() const noexcept { return m_Impl->Pawns->TargetPopulation(); }

	std::size_t World::MaximumPopulation() const noexcept { return m_Impl->Pawns->MaximumPopulation(); }

	void World::SetTargetPopulation(const std::size_t population) { m_Impl->Pawns->SetTargetPopulation(population); }

	bool World::Fill(const Voxel::Api::Region region, const Voxel::Solid::Api::MaterialId material)
	{
		if (!region.IsValid() || region.IsEmpty() || !material.IsValid())
			return false;
		std::vector<Voxel::Solid::Api::Cell> cells(*region.CellCount());
		if (!m_Impl->Solids->ReadRegion(region, cells))
			return false;
		std::vector<Voxel::Solid::Api::Placement> placements;
		placements.reserve(cells.size());
		std::size_t index{};
		for (auto z = region.Min.Z; z < region.Max.Z; ++z)
			for (auto y = region.Min.Y; y < region.Max.Y; ++y)
				for (auto x = region.Min.X; x < region.Max.X; ++x)
					if (cells[index++].IsEmpty())
						placements.push_back({{x, y, z}, material});
		if (placements.empty())
			return true;
		const std::array commands{Voxel::Solid::Api::QueuedCommand{Voxel::Solid::Api::FillCommand{
			{m_Impl->Simulation->CurrentTick(), SolidToolSource, ++m_Impl->SolidSequence}, std::move(placements)}}};
		return m_Impl->SolidCommands->Submit(commands).has_value();
	}

	bool World::Erase(const Voxel::Api::Region region)
	{
		if (!region.IsValid() || region.IsEmpty())
			return false;
		std::vector<Voxel::Solid::Api::Cell> cells(*region.CellCount());
		if (!m_Impl->Solids->ReadRegion(region, cells))
			return false;
		std::vector<Voxel::Api::Position> positions;
		positions.reserve(cells.size());
		std::size_t index{};
		for (auto z = region.Min.Z; z < region.Max.Z; ++z)
			for (auto y = region.Min.Y; y < region.Max.Y; ++y)
				for (auto x = region.Min.X; x < region.Max.X; ++x)
					if (!cells[index++].IsEmpty())
						positions.push_back({x, y, z});
		if (positions.empty())
			return true;
		const std::array commands{Voxel::Solid::Api::QueuedCommand{Voxel::Solid::Api::EraseCommand{
			{m_Impl->Simulation->CurrentTick(), SolidToolSource, ++m_Impl->SolidSequence}, std::move(positions)}}};
		return m_Impl->SolidCommands->Submit(commands).has_value();
	}

	bool World::Navigate(const Ecs::Api::EntityId pawn, const Voxel::Api::Position destination)
	{
		const auto pawns = m_Impl->Pawns->Entities();
		if (std::ranges::find(pawns, pawn) == pawns.end())
			return false;
		constexpr auto one = Math::Api::FixedPointScalar::OneRaw;
		constexpr auto half = one / 2;
		const Spatial::Api::Position goal{
			Math::Api::FixedPointScalar::FromRaw(static_cast<std::int64_t>(destination.X) * one + half),
			Math::Api::FixedPointScalar::FromRaw(static_cast<std::int64_t>(destination.Y) * one + half),
			Math::Api::FixedPointScalar::FromRaw(static_cast<std::int64_t>(destination.Z) * one)};
		const std::array commands{Navigation::Api::Command{Navigation::Api::Start{
			{m_Impl->Simulation->CurrentTick(), ManualNavigationSource, ++m_Impl->NavigationSequence},
			pawn,
			Navigation::Api::ExecutionId{++m_Impl->NavigationExecution},
			{goal, Math::Api::FixedPointScalar::FromRaw(one / 4)}}}};
		const auto submitted = m_Impl->Following->Submit(commands).has_value();
		if (submitted)
			m_Impl->Pawns->TrackExternalNavigation(pawn);
		return submitted;
	}

	RuntimeStats World::Stats() const noexcept
	{
		RuntimeStats result{m_Impl->Simulation->CurrentTick(), m_Impl->Pawns->Entities().size()};
		for (const auto pawn : m_Impl->Pawns->Entities())
		{
			const auto execution = m_Impl->Entities->Get<Navigation::Api::NavigationExecutionComponent>(pawn);
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
		const auto* configuration = FindConfiguration(id);
		if (configuration == nullptr)
			throw std::invalid_argument{"Unknown testbed world: " + std::string{id}};
		return std::unique_ptr<World>{new World{std::make_unique<World::Impl>(*configuration, profiling)}};
	}
} // namespace UnrealVoxelSim::Testbed
