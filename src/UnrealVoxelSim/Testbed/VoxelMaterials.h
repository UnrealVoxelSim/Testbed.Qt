#pragma once

#include "UnrealVoxelSim/Voxel/Rendering/Api/SurfaceId.h"
#include "UnrealVoxelSim/Voxel/Rendering/Api/MaterialSurfaceBinding.h"
#include "UnrealVoxelSim/Voxel/Rendering/Api/SurfaceAppearance.h"
#include "UnrealVoxelSim/Voxel/Solid/Api/MaterialTraversal.h"

#include <span>
#include <string_view>

namespace UnrealVoxelSim::Testbed
{
	struct VoxelMaterialDefinition final
	{
		Voxel::Solid::Api::MaterialId Material{};
		std::string_view DisplayName;
		Voxel::Solid::Api::MaterialTraversal Traversal{};
		Voxel::Rendering::Api::SurfaceId Surface{};
		Voxel::Rendering::Api::SurfaceAppearance Appearance{};
	};

	[[nodiscard]] std::span<const VoxelMaterialDefinition> VoxelMaterialDefinitions() noexcept;
	[[nodiscard]] std::span<const Voxel::Rendering::Api::MaterialSurfaceBinding>
	VoxelMaterialSurfaceBindings() noexcept;
}
