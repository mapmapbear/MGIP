#include "render/MeshSDFQuality.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
	bool expect(bool condition, const char* message)
	{
		if (!condition)
		{
			std::cerr << "FAILED: " << message << '\n';
		}
		return condition;
	}

	bool nearlyEqual(float lhs, float rhs, float epsilon = 1.0e-3f)
	{
		return std::abs(lhs - rhs) <= epsilon;
	}
}

int main()
{
	bool passed = true;

	const glm::vec3 sourceExtent{151.147f, 634.447f, 395.177f};
	const float paddedGlobalExtent = sourceExtent.y * 1.02f;
	const glm::vec3 globalBoundsMax{paddedGlobalExtent};

	const demo::MeshSDFPhysicalResolutionQuality coarse =
		demo::evaluateMeshSDFPhysicalResolution(
			glm::vec3(0.0f), sourceExtent, glm::uvec3(64u),
			glm::vec3(0.0f), globalBoundsMax, 256u);
	passed &= expect(coarse.valid, "64^3 quality metrics are valid");
	passed &= expect(coarse.warningLevel == demo::MeshSDFQualityWarningLevel::severe,
	                 "64^3 source triggers the severe warning");
	passed &= expect(nearlyEqual(coarse.globalVoxelSize, 2.528f),
	                 "global voxel size uses the padded GlobalSDF extent");
	passed &= expect(nearlyEqual(coarse.sourceSpacing.x, 2.399f)
	              && nearlyEqual(coarse.sourceSpacing.y, 10.071f)
	              && nearlyEqual(coarse.sourceSpacing.z, 6.273f),
	                 "source spacing divides each bounds extent by resolution minus one");
	passed &= expect(nearlyEqual(coarse.ratio.x, 0.95f, 0.01f)
	              && nearlyEqual(coarse.ratio.y, 3.98f, 0.01f)
	              && nearlyEqual(coarse.ratio.z, 2.48f, 0.01f),
	                 "64^3 ratios match physical source spacing");

	const demo::MeshSDFPhysicalResolutionQuality adequate =
		demo::evaluateMeshSDFPhysicalResolution(
			glm::vec3(0.0f), sourceExtent, glm::uvec3(256u),
			glm::vec3(0.0f), globalBoundsMax, 256u);
	passed &= expect(adequate.valid, "256^3 quality metrics are valid");
	passed &= expect(adequate.warningLevel == demo::MeshSDFQualityWarningLevel::none,
	                 "256^3 source does not warn");
	passed &= expect(nearlyEqual(adequate.ratio.x, 0.23f, 0.01f)
	              && nearlyEqual(adequate.ratio.y, 0.98f, 0.01f)
	              && nearlyEqual(adequate.ratio.z, 0.61f, 0.01f),
	                 "256^3 ratios match physical source spacing");

	const demo::MeshSDFPhysicalResolutionQuality warning =
		demo::evaluateMeshSDFPhysicalResolution(
			glm::vec3(0.0f), glm::vec3(1.5f, 1.0f, 1.0f), glm::uvec3(2u),
			glm::vec3(0.0f), glm::vec3(1.0f), 1u);
	passed &= expect(warning.warningLevel == demo::MeshSDFQualityWarningLevel::warning,
	                 "ratio above 1.25 triggers a normal warning");

	const demo::MeshSDFPhysicalResolutionQuality warningBoundary =
		demo::evaluateMeshSDFPhysicalResolution(
			glm::vec3(0.0f), glm::vec3(1.25f), glm::uvec3(2u),
			glm::vec3(0.0f), glm::vec3(1.0f), 1u);
	passed &= expect(warningBoundary.warningLevel == demo::MeshSDFQualityWarningLevel::none,
	                 "ratio equal to 1.25 does not warn");

	const demo::MeshSDFPhysicalResolutionQuality severeBoundary =
		demo::evaluateMeshSDFPhysicalResolution(
			glm::vec3(0.0f), glm::vec3(2.0f, 1.0f, 1.0f), glm::uvec3(2u),
			glm::vec3(0.0f), glm::vec3(1.0f), 1u);
	passed &= expect(severeBoundary.warningLevel == demo::MeshSDFQualityWarningLevel::warning,
	                 "ratio equal to 2.0 remains a normal warning");

	const demo::MeshSDFPhysicalResolutionQuality invalid =
		demo::evaluateMeshSDFPhysicalResolution(
			glm::vec3(0.0f), glm::vec3(1.0f), glm::uvec3(1u, 64u, 64u),
			glm::vec3(0.0f), glm::vec3(1.0f), 256u);
	passed &= expect(!invalid.valid
	              && invalid.warningLevel == demo::MeshSDFQualityWarningLevel::none,
	                 "invalid source resolution is ignored");

	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
