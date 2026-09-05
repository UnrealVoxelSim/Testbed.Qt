#include "VoxelMaterials.h"

#include "UnrealVoxelSim/Voxel/Solid/Api/StandardMaterials.h"

#include <array>

namespace UnrealVoxelSim::Testbed
{
	namespace
	{
		using Voxel::Rendering::Api::SurfaceId;
		using Voxel::Rendering::Api::MaterialSurfaceBinding;
		using Voxel::Rendering::Api::SurfaceAppearance;
		using Voxel::Rendering::Api::TextureKey;
		using Voxel::Solid::Api::MaterialTraversal;
		using Voxel::Solid::Api::StandardMaterials::Dirt;
		using Voxel::Solid::Api::StandardMaterials::Grass;
		using Voxel::Solid::Api::StandardMaterials::Leaves;
		using Voxel::Solid::Api::StandardMaterials::Plank;
		using Voxel::Solid::Api::StandardMaterials::Stone;
		using Voxel::Solid::Api::StandardMaterials::Trunk;

		constexpr SurfaceAppearance All(const TextureKey texture) noexcept
		{
			return SurfaceAppearance{{texture, texture, texture, texture, texture, texture}};
		}

		constexpr SurfaceAppearance TopSidesBottom(const TextureKey bottom,
		                                           const TextureKey sides,
		                                           const TextureKey top) noexcept
		{
			return SurfaceAppearance{{sides, sides, sides, sides, bottom, top}};
		}

		constexpr TextureKey DirtTexture{"dirt"};
		constexpr TextureKey DirtGrassTexture{"dirt_grass"};
		constexpr TextureKey GrassTopTexture{"grass_top"};
		// TODO Use "C:\Users\DmitriyPC\Downloads\kenney_voxel-pack\PNG\Tiles\leaves.png" or "C:\Users\DmitriyPC\Downloads\kenney_voxel-pack\PNG\Tiles\leaves_transparent.png"
		constexpr TextureKey LeavesTexture{"grass_top"};
		constexpr TextureKey StoneTexture{"stone"};
		constexpr TextureKey TrunkSideTexture{"trunk_side"};
		constexpr TextureKey TrunkTopTexture{"trunk_top"};
		constexpr TextureKey WoodTexture{"wood"};

		constexpr std::array Definitions{
			VoxelMaterialDefinition{
				.Material = Dirt,
				.DisplayName = "Dirt",
				.Traversal = MaterialTraversal{Dirt},
				.Surface = SurfaceId{Dirt.Value()},
				.Appearance = All(DirtTexture),
			},
			VoxelMaterialDefinition{
				.Material = Grass,
				.DisplayName = "Grass",
				.Traversal = MaterialTraversal{Grass},
				.Surface = SurfaceId{Grass.Value()},
				.Appearance = TopSidesBottom(DirtTexture, DirtGrassTexture, GrassTopTexture),
			},
			VoxelMaterialDefinition{
				.Material = Stone,
				.DisplayName = "Stone",
				.Traversal = MaterialTraversal{Stone},
				.Surface = SurfaceId{Stone.Value()},
				.Appearance = All(StoneTexture),
			},
			VoxelMaterialDefinition{
				.Material = Trunk,
				.DisplayName = "Trunk",
				.Traversal = MaterialTraversal{Trunk},
				.Surface = SurfaceId{Trunk.Value()},
				.Appearance = TopSidesBottom(TrunkTopTexture, TrunkSideTexture, TrunkTopTexture),
			},
			VoxelMaterialDefinition{
				.Material = Plank,
				.DisplayName = "Plank",
				.Traversal = MaterialTraversal{Plank},
				.Surface = SurfaceId{Plank.Value()},
				.Appearance = All(WoodTexture),
			},
			VoxelMaterialDefinition{
				.Material = Leaves,
				.DisplayName = "Leaves",
				.Traversal = MaterialTraversal{Leaves},
				.Surface = SurfaceId{Leaves.Value()},
				.Appearance = All(LeavesTexture),
			},
		};

		constexpr std::array Bindings{
			MaterialSurfaceBinding{Dirt, SurfaceId{Dirt.Value()}},
			MaterialSurfaceBinding{Grass, SurfaceId{Grass.Value()}},
			MaterialSurfaceBinding{Stone, SurfaceId{Stone.Value()}},
			MaterialSurfaceBinding{Trunk, SurfaceId{Trunk.Value()}},
			MaterialSurfaceBinding{Plank, SurfaceId{Plank.Value()}},
			MaterialSurfaceBinding{Leaves, SurfaceId{Leaves.Value()}},
		};
	}

	std::span<const VoxelMaterialDefinition> VoxelMaterialDefinitions() noexcept
	{
		return Definitions;
	}

	std::span<const Voxel::Rendering::Api::MaterialSurfaceBinding> VoxelMaterialSurfaceBindings() noexcept
	{
		return Bindings;
	}
}
