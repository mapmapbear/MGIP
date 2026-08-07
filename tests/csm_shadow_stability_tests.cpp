#include "../render/CSMShadowResources.h"
#include "../render/ClipSpaceConvention.h"
#include "../render/RenderTypes.h"
#include "../render/GPUSceneRegistry.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
	const float kFieldOfViewRadians = glm::radians(35.0f);
	constexpr float kAspectRatio = 16.0f / 9.0f;
	constexpr float kNearPlane = 0.1f;
	constexpr float kFarPlane = 1000.0f;
	constexpr float kShadowDistance = 100.0f;
	constexpr double kLightBasisRegularizationInnerChord =
		16.0 * static_cast<double>(std::numeric_limits<float>::epsilon());
	constexpr double kLightBasisRegularizationOuterChord =
		1024.0 * static_cast<double>(std::numeric_limits<float>::epsilon());
	const glm::vec3 kCasterBoundsMin(-20.0f, -2.0f, -20.0f);
	const glm::vec3 kCasterBoundsMax(20.0f, 18.0f, 20.0f);

	void expect(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	[[nodiscard]] bool nearlyEqual(float lhs, float rhs, float epsilon)
	{
		return std::abs(lhs - rhs) <= epsilon;
	}

	[[nodiscard]] bool matricesNearlyEqual(
		const glm::mat4& lhs,
		const glm::mat4& rhs,
		float epsilon)
	{
		for (uint32_t column = 0; column < 4; ++column)
		{
			for (uint32_t row = 0; row < 4; ++row)
			{
				if (!nearlyEqual(lhs[column][row], rhs[column][row], epsilon))
				{
					return false;
				}
			}
		}
		return true;
	}

	[[nodiscard]] bool vectorsNearlyEqual(const glm::vec3& lhs, const glm::vec3& rhs, float epsilon)
	{
		return nearlyEqual(lhs.x, rhs.x, epsilon)
			&& nearlyEqual(lhs.y, rhs.y, epsilon)
			&& nearlyEqual(lhs.z, rhs.z, epsilon);
	}

	[[nodiscard]] bool vectorsNearlyEqual(const glm::vec4& lhs, const glm::vec4& rhs, float epsilon)
	{
		return nearlyEqual(lhs.x, rhs.x, epsilon)
			&& nearlyEqual(lhs.y, rhs.y, epsilon)
			&& nearlyEqual(lhs.z, rhs.z, epsilon)
			&& nearlyEqual(lhs.w, rhs.w, epsilon);
	}

	[[nodiscard]] bool matrixIsFinite(const glm::mat4& matrix)
	{
		for (uint32_t column = 0; column < 4; ++column)
		{
			for (uint32_t row = 0; row < 4; ++row)
			{
				if (!std::isfinite(matrix[column][row]))
				{
					return false;
				}
			}
		}
		return true;
	}

	struct LightFrameAxes
	{
		glm::vec3 right{0.0f};
		glm::vec3 up{0.0f};
		glm::vec3 back{0.0f};
	};

	struct LightDirectionBasisSample
	{
		glm::vec3 direction{0.0f};
		LightFrameAxes axes{};
	};

	[[nodiscard]] LightFrameAxes extractLightFrameAxes(const glm::mat4& lightView)
	{
		const glm::mat4 lightToWorld = glm::inverse(lightView);
		return LightFrameAxes{
			glm::normalize(glm::vec3(lightToWorld[0])),
			glm::normalize(glm::vec3(lightToWorld[1])),
			glm::normalize(glm::vec3(lightToWorld[2])),
		};
	}

	void verifyLightFrameAxes(const glm::mat4& lightView)
	{
		expect(matrixIsFinite(lightView), "light view matrix contains a non-finite value");
		const LightFrameAxes axes = extractLightFrameAxes(lightView);
		expect(nearlyEqual(glm::length(axes.right), 1.0f, 1.0e-5f)
		       && nearlyEqual(glm::length(axes.up), 1.0f, 1.0e-5f)
		       && nearlyEqual(glm::length(axes.back), 1.0f, 1.0e-5f),
		       "light view basis is not normalized");
		expect(std::abs(glm::dot(axes.right, axes.up)) <= 1.0e-5f
		       && std::abs(glm::dot(axes.right, axes.back)) <= 1.0e-5f
		       && std::abs(glm::dot(axes.up, axes.back)) <= 1.0e-5f,
		       "light view basis is not orthogonal");
		expect(glm::dot(glm::cross(axes.right, axes.up), axes.back) > 0.9999f,
		       "light view basis changed handedness");
	}

	float g_maxProjectionSingularityAdjacentAngleRadians = 0.0f;
	float g_maxZeroProjectionAnalyticAngleRadians = 0.0f;
	float g_maxChartOverlapTeleportPairAngleRadians = 0.0f;
	float g_maxLargeTeleportNeighborhoodAdjacentAngleRadians = 0.0f;
	float g_maxRandomSpherePathAdjacentAngleRadians = 0.0f;
	float g_maxClosedLoopAdjacentAngleRadians = 0.0f;
	float g_maxRegularizationComponentUlpAngleRadians = 0.0f;
	float g_maxRegularizationBoundaryRadialAngleRadians = 0.0f;
	float g_maxRegularizationBoundaryAzimuthAngleRadians = 0.0f;
	float g_maxRegularizationRandomUlpAngleRadians = 0.0f;

	[[nodiscard]] float unitVectorAngleRadians(const glm::vec3& lhs, const glm::vec3& rhs)
	{
		const glm::dvec3 lhsDouble(lhs);
		const glm::dvec3 rhsDouble(rhs);
		return static_cast<float>(std::atan2(
			glm::length(glm::cross(lhsDouble, rhsDouble)), glm::dot(lhsDouble, rhsDouble)));
	}

	[[nodiscard]] float maxLightBasisAngleRadians(
		const LightFrameAxes& lhs,
		const LightFrameAxes& rhs)
	{
		return std::max(
			unitVectorAngleRadians(lhs.up, rhs.up),
			unitVectorAngleRadians(lhs.right, rhs.right));
	}

	[[nodiscard]] glm::vec3 directionWithVerticalComponent(float y, float azimuthRadians)
	{
		const float horizontal = std::sqrt(std::max(1.0f - y * y, 0.0f));
		return glm::vec3(
			horizontal * std::cos(azimuthRadians),
			y,
			horizontal * std::sin(azimuthRadians));
	}
	[[nodiscard]] shaderio::CameraUniforms makeCameraWithProjection(		const glm::vec3& position,
		const glm::vec3& forward,
		const glm::mat4& projection)
	{
		shaderio::CameraUniforms camera{};
		camera.view = glm::lookAt(
			position,
			position + glm::normalize(forward),
			glm::vec3(0.0f, 1.0f, 0.0f));
		camera.projection = projection;
		camera.viewProjection = camera.projection * camera.view;
		camera.inverseViewProjection = glm::inverse(camera.viewProjection);
		camera.unjitteredViewProjection = camera.viewProjection;
		camera.unjitteredInverseViewProjection = camera.inverseViewProjection;
		camera.cameraPosition = position;
		return camera;
	}

	[[nodiscard]] shaderio::CameraUniforms makeCamera(
		const glm::vec3& position,
		const glm::vec3& forward)
	{
		const demo::clipspace::ProjectionConvention convention =
			demo::clipspace::getProjectionConvention(demo::clipspace::BackendConvention::vulkan);
		const glm::mat4 projection = demo::clipspace::makePerspectiveProjection(
			kFieldOfViewRadians,
			kAspectRatio,
			kNearPlane,
			kFarPlane,
			convention);
		return makeCameraWithProjection(position, forward, projection);
	}

	struct NearAntipodalBasisSample
	{
		float realizedDirectionDot{0.0f};
		LightFrameAxes historyAxes{};
		LightFrameAxes axes{};
	};

	[[nodiscard]] NearAntipodalBasisSample evaluateNearAntipodalBasis(
		const shaderio::CameraUniforms& camera,
		float requestedDirectionDot)
	{
		const glm::vec3 initialDirection =
			glm::normalize(glm::vec3(0.10f, -0.96f, -0.25f));
		const glm::vec3 transportedDirection =
			glm::normalize(glm::vec3(0.65f, -0.55f, -0.52f));
		demo::CSMShadowResources csm;
		csm.updateCascadeMatrices(camera, initialDirection * 11.0f, kShadowDistance);
		csm.updateCascadeMatrices(camera, transportedDirection * 0.125f, kShadowDistance);

		const auto& historyFrame = csm.getFrameData();
		verifyLightFrameAxes(historyFrame.cascades[0].lightView);
		const glm::vec3 historyDirection = historyFrame.lightDirection;
		const LightFrameAxes historyAxes =
			extractLightFrameAxes(historyFrame.cascades[0].lightView);
		const glm::vec3 absoluteHistoryUp = glm::abs(historyAxes.up);
		expect(absoluteHistoryUp.x < 0.95f
		       && absoluteHistoryUp.y < 0.95f
		       && absoluteHistoryUp.z < 0.95f,
		       "near-antipodal regression fixture did not create a non-axis history up");

		glm::vec3 sweepTangent =
			historyAxes.right - historyDirection * glm::dot(historyAxes.right, historyDirection);
		sweepTangent = glm::normalize(sweepTangent);
		const float clampedDirectionDot = glm::clamp(requestedDirectionDot, -1.0f, 1.0f);
		const float tangentMagnitude =
			std::sqrt(std::max(1.0f - clampedDirectionDot * clampedDirectionDot, 0.0f));
		const glm::vec3 targetDirection = glm::normalize(
			historyDirection * clampedDirectionDot + sweepTangent * tangentMagnitude);
		const float realizedDirectionDot = glm::dot(historyDirection, targetDirection);
		expect(nearlyEqual(realizedDirectionDot, clampedDirectionDot, 2.0e-6f),
		       "near-antipodal fixture did not realize the requested direction dot");

		csm.updateCascadeMatrices(camera, targetDirection * 7.0f, kShadowDistance);
		const auto& targetFrame = csm.getFrameData();
		verifyLightFrameAxes(targetFrame.cascades[0].lightView);
		return NearAntipodalBasisSample{
			realizedDirectionDot,
			historyAxes,
			extractLightFrameAxes(targetFrame.cascades[0].lightView),
		};
	}

	struct ProjectionSingularityBasisSample
	{
		float realizedDirectionDot{0.0f};
		float realizedProjectedLengthSq{0.0f};
		glm::vec3 realizedDirection{0.0f};
		LightFrameAxes historyAxes{};
		LightFrameAxes axes{};
	};

	[[nodiscard]] ProjectionSingularityBasisSample evaluateProjectionSingularityBasis(
		const shaderio::CameraUniforms& camera,
		float projectionTangentMagnitude,
		float requestedDirectionDot,
		float historyUpSign,
		bool primeProjectionChart)
	{
		expect(historyUpSign == -1.0f || historyUpSign == 1.0f,
		       "projection singularity fixture requires an exact history-up sign");
		const glm::vec3 initialDirection(0.0f, -1.0f, 0.0f);
		demo::CSMShadowResources csm;
		csm.updateCascadeMatrices(camera, initialDirection, kShadowDistance);
		if (primeProjectionChart)
		{
			csm.updateCascadeMatrices(camera, -initialDirection, kShadowDistance);
			csm.updateCascadeMatrices(camera, initialDirection, kShadowDistance);
		}

		const auto& historyFrame = csm.getFrameData();
		verifyLightFrameAxes(historyFrame.cascades[0].lightView);
		const glm::vec3 historyDirection = historyFrame.lightDirection;
		const LightFrameAxes historyAxes =
			extractLightFrameAxes(historyFrame.cascades[0].lightView);
		expect(vectorsNearlyEqual(historyDirection, initialDirection, 1.0e-7f)
		       && vectorsNearlyEqual(historyAxes.up, glm::vec3(1.0f, 0.0f, 0.0f), 1.0e-7f),
		       "projection singularity fixture did not create the exact canonical history basis");

		const float tangentLengthSq =
			projectionTangentMagnitude * projectionTangentMagnitude
			+ requestedDirectionDot * requestedDirectionDot;
		expect(std::isfinite(tangentLengthSq) && tangentLengthSq < 1.0f,
		       "projection singularity fixture requested an invalid tangent length");
		const float historyUpComponent =
			historyUpSign * std::sqrt(std::max(1.0f - tangentLengthSq, 0.0f));
		const glm::vec3 targetDirection =
			historyAxes.up * historyUpComponent
			+ historyDirection * requestedDirectionDot
			+ historyAxes.right * projectionTangentMagnitude;
		csm.updateCascadeMatrices(camera, targetDirection, kShadowDistance);

		const auto& targetFrame = csm.getFrameData();
		verifyLightFrameAxes(targetFrame.cascades[0].lightView);
		const glm::vec3 realizedDirection = targetFrame.lightDirection;
		const glm::vec3 previousUpProjection =
			historyAxes.up - realizedDirection * glm::dot(historyAxes.up, realizedDirection);
		return ProjectionSingularityBasisSample{
			glm::dot(historyDirection, realizedDirection),
			glm::dot(previousUpProjection, previousUpProjection),
			realizedDirection,
			historyAxes,
			extractLightFrameAxes(targetFrame.cascades[0].lightView),
		};
	}

	struct SameHistoryTargetBasisSample
	{
		glm::vec3 realizedDirection{0.0f};
		LightFrameAxes historyAxes{};
		LightFrameAxes axes{};
	};

	struct ScaleInvariantPoleBasisSample
	{
		glm::vec3 historyDirection{0.0f};
		LightFrameAxes historyAxes{};
		glm::vec3 realizedDirection{0.0f};
		LightFrameAxes axes{};
	};

	[[nodiscard]] SameHistoryTargetBasisSample evaluateCanonicalSameHistoryTarget(
		const shaderio::CameraUniforms& camera,
		float historyDirectionCoefficient,
		float historyUpCoefficient,
		float historyRightCoefficient,
		bool primeAntipodalChart)
	{
		const glm::vec3 initialDirection(0.0f, -1.0f, 0.0f);
		demo::CSMShadowResources csm;
		csm.updateCascadeMatrices(camera, initialDirection, kShadowDistance);
		if (primeAntipodalChart)
		{
			// Exact antipodal edits retain the canonical basis while selecting the
			// complementary chart for this identical persisted history.
			csm.updateCascadeMatrices(camera, -initialDirection, kShadowDistance);
			csm.updateCascadeMatrices(camera, initialDirection, kShadowDistance);
		}

		const auto& historyFrame = csm.getFrameData();
		verifyLightFrameAxes(historyFrame.cascades[0].lightView);
		const LightFrameAxes historyAxes =
			extractLightFrameAxes(historyFrame.cascades[0].lightView);
		expect(vectorsNearlyEqual(historyFrame.lightDirection, initialDirection, 1.0e-7f)
		       && vectorsNearlyEqual(historyAxes.up, glm::vec3(1.0f, 0.0f, 0.0f), 1.0e-7f),
		       "same-history fixture did not create the canonical persisted basis");

		const glm::vec3 targetDirection = glm::normalize(
			historyFrame.lightDirection * historyDirectionCoefficient
			+ historyAxes.up * historyUpCoefficient
			+ historyAxes.right * historyRightCoefficient);
		csm.updateCascadeMatrices(camera, targetDirection, kShadowDistance);
		const auto& targetFrame = csm.getFrameData();
		verifyLightFrameAxes(targetFrame.cascades[0].lightView);
		return SameHistoryTargetBasisSample{
			targetFrame.lightDirection,
			historyAxes,
			extractLightFrameAxes(targetFrame.cascades[0].lightView),
		};
	}

	[[nodiscard]] ScaleInvariantPoleBasisSample evaluateScaleInvariantPoleBasis(
		const shaderio::CameraUniforms& camera,
		const glm::vec3& historySetupDirection,
		float targetScale,
		bool primeAntipodalChart,
		bool targetAntipode,
		float historyUpCoefficient = 0.0f,
		float historyRightCoefficient = 0.0f)
	{
		expect(std::isfinite(targetScale) && targetScale > 0.0f,
		       "scale-invariant pole fixture requires a positive finite target scale");
		demo::CSMShadowResources csm;
		if (primeAntipodalChart)
		{
			const glm::vec3 canonicalDirection(0.0f, -1.0f, 0.0f);
			csm.updateCascadeMatrices(camera, canonicalDirection, kShadowDistance);
			csm.updateCascadeMatrices(camera, -canonicalDirection, kShadowDistance);
			csm.updateCascadeMatrices(camera, canonicalDirection, kShadowDistance);
		}
		csm.updateCascadeMatrices(camera, historySetupDirection, kShadowDistance);

		const auto& historyFrame = csm.getFrameData();
		verifyLightFrameAxes(historyFrame.cascades[0].lightView);
		const glm::vec3 historyDirection = historyFrame.lightDirection;
		const LightFrameAxes historyAxes =
			extractLightFrameAxes(historyFrame.cascades[0].lightView);
		const glm::vec3 unscaledTargetDirection =
			historyDirection * (targetAntipode ? -1.0f : 1.0f)
			+ historyAxes.up * historyUpCoefficient
			+ historyAxes.right * historyRightCoefficient;
		const glm::vec3 targetDirection = unscaledTargetDirection * targetScale;
		csm.updateCascadeMatrices(camera, targetDirection, kShadowDistance);

		const auto& targetFrame = csm.getFrameData();
		verifyLightFrameAxes(targetFrame.cascades[0].lightView);
		return ScaleInvariantPoleBasisSample{
			historyDirection,
			historyAxes,
			targetFrame.lightDirection,
			extractLightFrameAxes(targetFrame.cascades[0].lightView),
		};
	}

	[[nodiscard]] ScaleInvariantPoleBasisSample evaluateExplicitPoleTargetBasis(
		const shaderio::CameraUniforms& camera,
		const glm::vec3& historySetupDirection,
		bool primeAntipodalChart,
		const glm::vec3& targetDirection)
	{
		demo::CSMShadowResources csm;
		if (primeAntipodalChart)
		{
			const glm::vec3 canonicalDirection(0.0f, -1.0f, 0.0f);
			csm.updateCascadeMatrices(camera, canonicalDirection, kShadowDistance);
			csm.updateCascadeMatrices(camera, -canonicalDirection, kShadowDistance);
			csm.updateCascadeMatrices(camera, canonicalDirection, kShadowDistance);
		}
		csm.updateCascadeMatrices(camera, historySetupDirection, kShadowDistance);

		const auto& historyFrame = csm.getFrameData();
		verifyLightFrameAxes(historyFrame.cascades[0].lightView);
		const glm::vec3 historyDirection = historyFrame.lightDirection;
		const LightFrameAxes historyAxes =
			extractLightFrameAxes(historyFrame.cascades[0].lightView);
		csm.updateCascadeMatrices(camera, targetDirection, kShadowDistance);

		const auto& targetFrame = csm.getFrameData();
		verifyLightFrameAxes(targetFrame.cascades[0].lightView);
		return ScaleInvariantPoleBasisSample{
			historyDirection,
			historyAxes,
			targetFrame.lightDirection,
			extractLightFrameAxes(targetFrame.cascades[0].lightView),
		};
	}


	[[nodiscard]] double selectedPoleChordLength(
		const ScaleInvariantPoleBasisSample& sample,
		bool targetAntipode)
	{
		const glm::dvec3 historyDirection =
			glm::normalize(glm::dvec3(sample.historyDirection));
		const glm::dvec3 realizedDirection =
			glm::normalize(glm::dvec3(sample.realizedDirection));
		const glm::dvec3 poleDirection =
			targetAntipode ? -historyDirection : historyDirection;
		return glm::length(realizedDirection - poleDirection);
	}

	struct ChartOverlapTeleportSample
	{
		float realizedDirectionDot{0.0f};
		float realizedTransportedLengthSq{0.0f};
		float realizedChartAlignment{0.0f};
		float realizedPlaneNormal{0.0f};
		glm::vec3 realizedDirection{0.0f};
		glm::vec3 transportedUp{0.0f};
		glm::vec3 projectedUp{0.0f};
		LightFrameAxes historyAxes{};
		LightFrameAxes axes{};
	};

	[[nodiscard]] ChartOverlapTeleportSample evaluateChartOverlapTeleport(
		const shaderio::CameraUniforms& camera,
		float requestedDirectionDot,
		float chartPlaneNormal,
		bool primeProjectionChart)
	{
		const glm::vec3 initialDirection(0.0f, -1.0f, 0.0f);
		demo::CSMShadowResources csm;
		csm.updateCascadeMatrices(camera, initialDirection, kShadowDistance);
		if (primeProjectionChart)
		{
			// Two exact antipodal edits return to the same history basis while selecting
			// the projection chart in a full-overlap, output-independent state transition.
			csm.updateCascadeMatrices(camera, -initialDirection, kShadowDistance);
			csm.updateCascadeMatrices(camera, initialDirection, kShadowDistance);
		}

		const auto& historyFrame = csm.getFrameData();
		verifyLightFrameAxes(historyFrame.cascades[0].lightView);
		const glm::vec3 historyDirection = historyFrame.lightDirection;
		const LightFrameAxes historyAxes =
			extractLightFrameAxes(historyFrame.cascades[0].lightView);
		expect(vectorsNearlyEqual(historyDirection, initialDirection, 1.0e-7f)
		       && vectorsNearlyEqual(historyAxes.up, glm::vec3(1.0f, 0.0f, 0.0f), 1.0e-7f),
		       "chart-overlap fixture did not restore the canonical history basis");

		const float clampedDirectionDot = glm::clamp(requestedDirectionDot, -1.0f, 1.0f);
		const float historyUpComponent = std::sqrt(std::max(
			1.0f - clampedDirectionDot * clampedDirectionDot
				- chartPlaneNormal * chartPlaneNormal,
			0.0f));
		const glm::vec3 targetDirection = glm::normalize(
			historyDirection * clampedDirectionDot
			+ historyAxes.up * historyUpComponent
			+ historyAxes.right * chartPlaneNormal);
		csm.updateCascadeMatrices(camera, targetDirection, kShadowDistance);

		const auto& targetFrame = csm.getFrameData();
		verifyLightFrameAxes(targetFrame.cascades[0].lightView);
		const glm::vec3 realizedDirection = targetFrame.lightDirection;
		const float realizedDirectionDot = glm::dot(historyDirection, realizedDirection);
		const glm::vec3 projectedUpRaw =
			historyAxes.up - realizedDirection * glm::dot(historyAxes.up, realizedDirection);
		const glm::vec3 transportedUpRaw =
			historyAxes.up * std::max(1.0f + realizedDirectionDot, 0.0f)
			- (historyDirection + realizedDirection)
			* glm::dot(historyAxes.up, realizedDirection);
		const float projectedLengthSq = glm::dot(projectedUpRaw, projectedUpRaw);
		const float transportedLengthSq = glm::dot(transportedUpRaw, transportedUpRaw);
		expect(projectedLengthSq > 0.0f && transportedLengthSq > 0.0f,
		       "chart-overlap fixture unexpectedly reached a chart singularity");
		const glm::vec3 projectedUp = projectedUpRaw * glm::inversesqrt(projectedLengthSq);
		const glm::vec3 transportedUp = transportedUpRaw * glm::inversesqrt(transportedLengthSq);

		return ChartOverlapTeleportSample{
			realizedDirectionDot,
			transportedLengthSq,
			glm::dot(transportedUp, projectedUp),
			glm::dot(realizedDirection, historyAxes.right),
			realizedDirection,
			transportedUp,
			projectedUp,
			historyAxes,
			extractLightFrameAxes(targetFrame.cascades[0].lightView),
		};
	}

	void setPreviousCameraMatrices(
		shaderio::CameraUniforms& camera,
		const shaderio::CameraUniforms& previousCamera,
		const glm::mat4& previousJitteredViewProjection)
	{
		camera.prevView = previousCamera.view;
		camera.prevProjection = previousCamera.projection;
		camera.prevViewProjection = previousCamera.viewProjection;
		camera.prevUnjitteredViewProjection = previousCamera.unjitteredViewProjection;
		camera.prevJitteredViewProjection = previousJitteredViewProjection;
	}

	[[nodiscard]] float halton(uint32_t index, uint32_t base)
	{
		float result = 0.0f;
		float fraction = 1.0f;
		while (index > 0)
		{
			fraction /= static_cast<float>(base);
			result += fraction * static_cast<float>(index % base);
			index /= base;
		}
		return result;
	}

	[[nodiscard]] glm::vec2 haltonJitterNdc(uint32_t phase)
	{
		const glm::vec2 centered(halton(phase + 1u, 2u) - 0.5f, halton(phase + 1u, 3u) - 0.5f);
		return glm::vec2(centered.x * (2.0f / 1920.0f), centered.y * (2.0f / 1080.0f));
	}

	[[nodiscard]] float orthographicWidth(const glm::mat4& projection)	{
		return 2.0f / std::abs(projection[0][0]);
	}

	[[nodiscard]] float orthographicHeight(const glm::mat4& projection)
	{
		return 2.0f / std::abs(projection[1][1]);
	}

	[[nodiscard]] glm::vec2 shadowUv(const glm::mat4& worldToShadowTexture, const glm::vec3& worldPoint)
	{
		const glm::vec4 shadowPosition = worldToShadowTexture * glm::vec4(worldPoint, 1.0f);
		return glm::vec2(shadowPosition) / shadowPosition.w;
	}

	void applyTemporalProjectionJitter(
		shaderio::CameraUniforms& camera,
		const glm::vec2& jitterNdc)
	{
		if (demo::clipspace::isOrthographicProjection(camera.projection))
		{
			camera.projection[3][0] += jitterNdc.x;
			camera.projection[3][1] += jitterNdc.y;
		}
		else
		{
			camera.projection[2][0] += jitterNdc.x;
			camera.projection[2][1] += jitterNdc.y;
		}

		camera.viewProjection = camera.projection * camera.view;
		camera.inverseViewProjection = glm::inverse(camera.viewProjection);
	}

	void updateProductionCascades(
		demo::CSMShadowResources& csm,
		const shaderio::CameraUniforms& camera,
		const glm::vec3& lightDirection,
		float normalBiasWorld = 0.0f)
	{
		csm.updateCascadeMatrices(
			camera,
			lightDirection,
			kShadowDistance,
			kCasterBoundsMin,
			kCasterBoundsMax,
			true,
			normalBiasWorld);
	}

	void verifySingleTexelGrid(const demo::CSMShadowResources::FrameData& frameData, uint32_t resolution)
	{
		for (uint32_t cascadeIndex = 0; cascadeIndex < frameData.cascadeCount; ++cascadeIndex)
		{
			const demo::CSMShadowResources::CascadeData& cascade = frameData.cascades[cascadeIndex];
			const float width = orthographicWidth(cascade.lightProjection);
			const float height = orthographicHeight(cascade.lightProjection);
			const float expectedExtent = cascade.texelSize * static_cast<float>(resolution);
			const float tolerance = std::max(
				expectedExtent * std::numeric_limits<float>::epsilon() * 16.0f,
				std::numeric_limits<float>::epsilon() * 16.0f);

			expect(nearlyEqual(width, expectedExtent, tolerance),
			       "cascade projection width does not use the published texel grid");
			expect(nearlyEqual(height, expectedExtent, tolerance),
			       "cascade projection height does not use the published texel grid");
			expect(nearlyEqual(width, height, tolerance),
			       "cascade projection is not square");
		}
	}

	void verifyCascadeBlendCoverage(
		const shaderio::CameraUniforms& camera,
		const demo::CSMShadowResources::FrameData& frameData)
	{
		for (uint32_t cascadeIndex = 1; cascadeIndex < frameData.cascadeCount; ++cascadeIndex)
		{
			const float previousCascadeNear = cascadeIndex > 1
				? frameData.splitDistances[cascadeIndex - 2]
				: 0.0f;
			const float nominalNear = frameData.splitDistances[cascadeIndex - 1];
			const float previousCascadeLength = std::max(nominalNear - previousCascadeNear, 0.001f);
			const float blendWidth = std::max(
				previousCascadeLength * shaderio::LCascadeBlendRegion,
				shaderio::LCascadeBlendMinDistance);
			const float expectedReceiverNear = std::max(kNearPlane, nominalNear - blendWidth);
			const auto& cascade = frameData.cascades[cascadeIndex];

			for (uint32_t cornerIndex = 0; cornerIndex < 4; ++cornerIndex)
			{
				const glm::vec4 viewCorner =
					camera.view * glm::vec4(cascade.receiverCornersWorld[cornerIndex], 1.0f);
				const float viewDepth = std::abs(viewCorner.z / viewCorner.w);
				const float depthTolerance = std::max(expectedReceiverNear * 1.0e-4f, 1.0e-4f);
				expect(nearlyEqual(viewDepth, expectedReceiverNear, depthTolerance),
				       "next cascade does not begin at the shared blend start");

				const glm::vec2 uv = shadowUv(
					cascade.worldToShadowTexture,
					cascade.receiverCornersWorld[cornerIndex]);
				expect(uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f,
				       "next cascade does not cover its blend-start receiver corner");
			}
		}
	}

	[[nodiscard]] std::array<glm::vec3, 8> casterBoundsCorners()
	{
		return {
			glm::vec3(kCasterBoundsMin.x, kCasterBoundsMin.y, kCasterBoundsMin.z),
			glm::vec3(kCasterBoundsMax.x, kCasterBoundsMin.y, kCasterBoundsMin.z),
			glm::vec3(kCasterBoundsMin.x, kCasterBoundsMax.y, kCasterBoundsMin.z),
			glm::vec3(kCasterBoundsMax.x, kCasterBoundsMax.y, kCasterBoundsMin.z),
			glm::vec3(kCasterBoundsMin.x, kCasterBoundsMin.y, kCasterBoundsMax.z),
			glm::vec3(kCasterBoundsMax.x, kCasterBoundsMin.y, kCasterBoundsMax.z),
			glm::vec3(kCasterBoundsMin.x, kCasterBoundsMax.y, kCasterBoundsMax.z),
			glm::vec3(kCasterBoundsMax.x, kCasterBoundsMax.y, kCasterBoundsMax.z),
		};
	}

	[[nodiscard]] std::array<glm::vec3, 12> fixedWorldAnchors()
	{
		return {
			glm::vec3(kCasterBoundsMin.x, kCasterBoundsMin.y, kCasterBoundsMin.z),
			glm::vec3(kCasterBoundsMax.x, kCasterBoundsMin.y, kCasterBoundsMin.z),
			glm::vec3(kCasterBoundsMin.x, kCasterBoundsMax.y, kCasterBoundsMin.z),
			glm::vec3(kCasterBoundsMax.x, kCasterBoundsMax.y, kCasterBoundsMin.z),
			glm::vec3(kCasterBoundsMin.x, kCasterBoundsMin.y, kCasterBoundsMax.z),
			glm::vec3(kCasterBoundsMax.x, kCasterBoundsMin.y, kCasterBoundsMax.z),
			glm::vec3(kCasterBoundsMin.x, kCasterBoundsMax.y, kCasterBoundsMax.z),
			glm::vec3(kCasterBoundsMax.x, kCasterBoundsMax.y, kCasterBoundsMax.z),
			glm::vec3(0.0f, 0.0f, 0.0f),
			glm::vec3(7.25f, 3.5f, -11.0f),
			glm::vec3(-13.0f, 8.0f, 5.75f),
			glm::vec3(2.0f, 16.0f, 14.0f),
		};
	}

	void verifyCasterDepthAndCulling(const demo::CSMShadowResources::FrameData& frameData)
	{
		expect(frameData.casterBoundsValid, "production caster bounds were not accepted");
		const std::array<glm::vec3, 8> casterCorners = casterBoundsCorners();

		for (uint32_t cascadeIndex = 0; cascadeIndex < frameData.cascadeCount; ++cascadeIndex)
		{
			const auto& cascade = frameData.cascades[cascadeIndex];
			verifyLightFrameAxes(cascade.lightView);

			const auto verifySidePlanes = [&](const glm::vec3& point)
			{
				for (uint32_t planeIndex = 0; planeIndex < 4u; ++planeIndex)
				{
					const glm::vec4& plane = cascade.cullingPlanes[planeIndex];
					const float distance = glm::dot(glm::vec3(plane), point) + plane.w;
					expect(distance >= -2.0e-3f,
					       "cascade XY culling rejects the receiver footprint or its light extrusion");
				}
			};

			const std::array<float, 3> extrusionDistances{
				0.0f,
				cascade.depthRange * 0.5f,
				cascade.depthRange,
			};
			for (const glm::vec3& receiverCorner : cascade.receiverCornersWorld)
			{
				for (const float extrusionDistance : extrusionDistances)
				{
					verifySidePlanes(receiverCorner - frameData.lightDirection * extrusionDistance);
				}
			}

			// Full-scene caster bounds stabilize only light-space Z. Individual cascade
			// side planes are intentionally free to reject unrelated caster XY regions.
			for (const glm::vec3& casterCorner : casterCorners)
			{
				const glm::vec3 lightSpace = glm::vec3(cascade.lightView * glm::vec4(casterCorner, 1.0f));
				const float lightDepth = -lightSpace.z;
				expect(lightDepth >= cascade.nearPlane - 1.0e-3f
				       && lightDepth <= cascade.farPlane + 1.0e-3f,
				       "cascade near/far planes do not contain a scene caster point");

				for (uint32_t planeIndex = 4u; planeIndex < cascade.cullingPlanes.size(); ++planeIndex)
				{
					const glm::vec4& plane = cascade.cullingPlanes[planeIndex];
					const float distance = glm::dot(glm::vec3(plane), casterCorner) + plane.w;
					expect(distance >= -2.0e-3f,
					       "cascade depth culling plane rejects a scene caster point");
				}
			}
		}
	}

	void verifyCascadeFramesEqual(
		const demo::CSMShadowResources::FrameData& expected,
		const demo::CSMShadowResources::FrameData& actual)
	{
		expect(expected.cascadeCount == actual.cascadeCount,
		       "temporal jitter changed the cascade count");
		for (uint32_t cascadeIndex = 0; cascadeIndex < expected.cascadeCount; ++cascadeIndex)
		{
			const auto& expectedCascade = expected.cascades[cascadeIndex];
			const auto& actualCascade = actual.cascades[cascadeIndex];
			expect(nearlyEqual(expected.splitDistances[cascadeIndex],
			                   actual.splitDistances[cascadeIndex],
			                   1.0e-5f),
			       "temporal jitter changed a cascade split");
			expect(nearlyEqual(expectedCascade.texelSize, actualCascade.texelSize, 1.0e-6f),
			       "temporal jitter changed a cascade texel size");
			expect(matricesNearlyEqual(expectedCascade.lightView,
			                           actualCascade.lightView,
			                           1.0e-5f),
			       "temporal jitter changed a cascade light view matrix");
			expect(matricesNearlyEqual(expectedCascade.lightProjection,
			                           actualCascade.lightProjection,
			                           1.0e-5f),
			       "temporal jitter changed a cascade light projection matrix");
			expect(matricesNearlyEqual(expectedCascade.viewProjection,
			                           actualCascade.viewProjection,
			                           1.0e-5f),
			       "temporal jitter changed a cascade view-projection matrix");
			expect(matricesNearlyEqual(expectedCascade.worldToShadowTexture,
			                           actualCascade.worldToShadowTexture,
			                           1.0e-5f),
			       "temporal jitter changed a cascade sampling matrix");
		}
	}

	void verifyCompleteCascadeFramesEqual(
		const demo::CSMShadowResources::FrameData& expected,
		const demo::CSMShadowResources::FrameData& actual,
		const char* message)
	{
		verifyCascadeFramesEqual(expected, actual);
		expect(vectorsNearlyEqual(expected.splitDistances, actual.splitDistances, 1.0e-5f), message);
		expect(vectorsNearlyEqual(expected.lightDirection, actual.lightDirection, 1.0e-6f), message);
		expect(nearlyEqual(expected.maxShadowDistance, actual.maxShadowDistance, 1.0e-5f), message);
		expect(vectorsNearlyEqual(expected.casterBoundsMin, actual.casterBoundsMin, 1.0e-6f), message);
		expect(vectorsNearlyEqual(expected.casterBoundsMax, actual.casterBoundsMax, 1.0e-6f), message);
		expect(expected.casterBoundsValid == actual.casterBoundsValid, message);

		for (uint32_t cascadeIndex = 0; cascadeIndex < expected.cascadeCount; ++cascadeIndex)
		{
			const auto& a = expected.cascades[cascadeIndex];
			const auto& b = actual.cascades[cascadeIndex];
			expect(nearlyEqual(a.splitNear, b.splitNear, 1.0e-5f), message);
			expect(nearlyEqual(a.splitFar, b.splitFar, 1.0e-5f), message);
			expect(nearlyEqual(a.receiverRadius, b.receiverRadius, 1.0e-5f), message);
			expect(nearlyEqual(a.nearPlane, b.nearPlane, 1.0e-5f), message);
			expect(nearlyEqual(a.farPlane, b.farPlane, 1.0e-5f), message);
			expect(vectorsNearlyEqual(a.receiverCenter, b.receiverCenter, 1.0e-5f), message);
			expect(vectorsNearlyEqual(a.lightPosition, b.lightPosition, 1.0e-5f), message);
			expect(vectorsNearlyEqual(a.receiverMinLightSpace, b.receiverMinLightSpace, 1.0e-5f), message);
			expect(vectorsNearlyEqual(a.receiverMaxLightSpace, b.receiverMaxLightSpace, 1.0e-5f), message);
			expect(vectorsNearlyEqual(a.casterMinLightSpace, b.casterMinLightSpace, 1.0e-5f), message);
			expect(vectorsNearlyEqual(a.casterMaxLightSpace, b.casterMaxLightSpace, 1.0e-5f), message);
			expect(matricesNearlyEqual(a.cullingViewProjection, b.cullingViewProjection, 1.0e-5f), message);
			for (size_t i = 0; i < a.receiverCornersWorld.size(); ++i)
			{
				expect(vectorsNearlyEqual(a.receiverCornersWorld[i], b.receiverCornersWorld[i], 1.0e-5f), message);
			}
			for (size_t i = 0; i < a.cullingPlanes.size(); ++i)
			{
				expect(vectorsNearlyEqual(a.cullingPlanes[i], b.cullingPlanes[i], 1.0e-5f), message);
			}
		}
	}

	void verifyFixedWorldAnchorsMoveByWholeTexels(
		const demo::CSMShadowResources::FrameData& baseFrame,
		const demo::CSMShadowResources::FrameData& movedFrame,
		uint32_t resolution,
		float tolerance,
		const char* message)
	{
		expect(baseFrame.cascadeCount == movedFrame.cascadeCount,
		       "temporal camera motion changed the cascade count");
		const std::array<glm::vec3, 12> anchors = fixedWorldAnchors();
		for (uint32_t cascadeIndex = 0; cascadeIndex < baseFrame.cascadeCount; ++cascadeIndex)
		{
			const auto& baseCascade = baseFrame.cascades[cascadeIndex];
			const auto& movedCascade = movedFrame.cascades[cascadeIndex];
			for (const glm::vec3& anchor : anchors)
			{
				const glm::vec2 baseUv = shadowUv(baseCascade.worldToShadowTexture, anchor);
				const glm::vec2 movedUv = shadowUv(movedCascade.worldToShadowTexture, anchor);
				const glm::vec2 texelDelta = (movedUv - baseUv) * static_cast<float>(resolution);
				expect(nearlyEqual(texelDelta.x, std::round(texelDelta.x), tolerance)
				       && nearlyEqual(texelDelta.y, std::round(texelDelta.y), tolerance), message);
			}
		}
	}

	[[nodiscard]] demo::CSMShadowResources::FrameData prepareTemporalFrameCascades(
		demo::CSMShadowResources& csm,
		const shaderio::CameraUniforms& currentUnjitteredCamera,
		const glm::vec3& lightDirection,
		const glm::vec2& jitterNdc)
	{
		updateProductionCascades(csm, currentUnjitteredCamera, lightDirection);
		const demo::CSMShadowResources::FrameData preparedFrame = csm.getFrameData();
		verifySingleTexelGrid(preparedFrame, csm.getCascadeResolution());

		shaderio::CameraUniforms temporalCamera = currentUnjitteredCamera;
		applyTemporalProjectionJitter(temporalCamera, jitterNdc);
		demo::RenderParams renderParams{};
		renderParams.cameraUniforms = &temporalCamera;
		renderParams.csmCascadeMatricesPrepared = true;
		expect(!demo::shouldUpdateCSMCascadeMatrices(renderParams),
		       "temporal jitter attempted to replace an unjittered CSM snapshot");
		if (demo::shouldUpdateCSMCascadeMatrices(renderParams))
		{
			updateProductionCascades(csm, temporalCamera, lightDirection);
		}
		verifyCompleteCascadeFramesEqual(preparedFrame, csm.getFrameData(),
		                                 "temporal jitter changed a prepared CSM result");
		return preparedFrame;
	}

	void verifyFixedWorldPointsMoveByWholeTexels(		const demo::CSMShadowResources::FrameData& baseFrame,
		const demo::CSMShadowResources::FrameData& movedFrame,
		uint32_t resolution,
		float tolerance,
		const char* message)
	{
		expect(baseFrame.cascadeCount == movedFrame.cascadeCount,
		       "camera motion changed the cascade count");
		const std::array<glm::vec3, 8> casterCorners = casterBoundsCorners();

		for (uint32_t cascadeIndex = 0; cascadeIndex < baseFrame.cascadeCount; ++cascadeIndex)
		{
			const auto& baseCascade = baseFrame.cascades[cascadeIndex];
			const auto& movedCascade = movedFrame.cascades[cascadeIndex];
			const auto verifyPoint = [&](const glm::vec3& worldPoint)
			{
				const glm::vec2 baseUv =
					shadowUv(baseCascade.worldToShadowTexture, worldPoint);
				const glm::vec2 movedUv =
					shadowUv(movedCascade.worldToShadowTexture, worldPoint);
				const glm::vec2 texelDelta =
					(movedUv - baseUv) * static_cast<float>(resolution);
				expect(nearlyEqual(texelDelta.x, std::round(texelDelta.x), tolerance)
				       && nearlyEqual(texelDelta.y, std::round(texelDelta.y), tolerance),
				       message);
			};

			for (const glm::vec3& receiverPoint : baseCascade.receiverCornersWorld)
			{
				verifyPoint(receiverPoint);
			}
			for (const glm::vec3& casterPoint : casterCorners)
			{
				verifyPoint(casterPoint);
			}
		}
	}

	void testStableCascadeProjection()
	{
		const glm::vec3 lightDirection = glm::normalize(glm::vec3(0.0f, -0.9848078f, 0.1736482f));
		const glm::vec3 cameraPosition(10.36013f, 5.722083f, 4.936913f);
		const glm::vec3 cameraForward = glm::normalize(glm::vec3(-0.62f, -0.18f, -0.76f));

		demo::CSMShadowResources csm;
		const shaderio::CameraUniforms camera = makeCamera(cameraPosition, cameraForward);
		updateProductionCascades(csm, camera, lightDirection);
		const demo::CSMShadowResources::FrameData baseFrame = csm.getFrameData();
		const uint32_t resolution = csm.getCascadeResolution();

		verifySingleTexelGrid(baseFrame, resolution);
		verifyCascadeBlendCoverage(camera, baseFrame);
		verifyCasterDepthAndCulling(baseFrame);

		const glm::mat4 firstLightToWorld = glm::inverse(baseFrame.cascades[0].lightView);
		const glm::vec3 lightSpaceRight = glm::normalize(glm::vec3(firstLightToWorld[0]));
		const glm::vec3 cameraDisplacement =
			lightSpaceRight * (baseFrame.cascades[0].texelSize * 3.25f);
		const shaderio::CameraUniforms translatedCamera =
			makeCamera(cameraPosition + cameraDisplacement, cameraForward);
		updateProductionCascades(csm, translatedCamera, lightDirection);
		const demo::CSMShadowResources::FrameData translatedFrame = csm.getFrameData();

		verifySingleTexelGrid(translatedFrame, resolution);
		verifyCascadeBlendCoverage(translatedCamera, translatedFrame);
		verifyCasterDepthAndCulling(translatedFrame);

		for (uint32_t cascadeIndex = 0; cascadeIndex < baseFrame.cascadeCount; ++cascadeIndex)
		{
			const auto& baseCascade = baseFrame.cascades[cascadeIndex];
			const auto& translatedCascade = translatedFrame.cascades[cascadeIndex];
			const float texelTolerance = std::max(baseCascade.texelSize * 1.0e-5f, 1.0e-6f);
			expect(nearlyEqual(baseCascade.texelSize, translatedCascade.texelSize, texelTolerance),
			       "camera translation changed the cascade texel size");

			const glm::vec2 baseUv =
				shadowUv(baseCascade.worldToShadowTexture, baseCascade.receiverCenter);
			const glm::vec2 translatedUv =
				shadowUv(translatedCascade.worldToShadowTexture, baseCascade.receiverCenter);
			const glm::vec2 texelDelta =
				(translatedUv - baseUv) * static_cast<float>(resolution);
			expect(nearlyEqual(texelDelta.x, std::round(texelDelta.x), 2.0e-3f)
			       && nearlyEqual(texelDelta.y, std::round(texelDelta.y), 2.0e-3f),
			       "camera translation moved the cascade by a fractional shadow texel");
		}
		verifyFixedWorldPointsMoveByWholeTexels(
			baseFrame,
			translatedFrame,
			resolution,
			3.0e-3f,
			"camera translation moved a fixed receiver or caster point by a fractional shadow texel");

		const shaderio::CameraUniforms rotatedCamera = makeCamera(
			cameraPosition,
			glm::normalize(glm::vec3(-0.21f, -0.33f, -0.92f)));
		updateProductionCascades(csm, rotatedCamera, lightDirection);
		const demo::CSMShadowResources::FrameData rotatedFrame = csm.getFrameData();

		verifySingleTexelGrid(rotatedFrame, resolution);
		verifyCascadeBlendCoverage(rotatedCamera, rotatedFrame);
		verifyCasterDepthAndCulling(rotatedFrame);

		for (uint32_t cascadeIndex = 0; cascadeIndex < baseFrame.cascadeCount; ++cascadeIndex)
		{
			const auto& baseCascade = baseFrame.cascades[cascadeIndex];
			const auto& rotatedCascade = rotatedFrame.cascades[cascadeIndex];
			const float tolerance = std::max(baseCascade.texelSize * 1.0e-5f, 1.0e-6f);
			expect(nearlyEqual(baseCascade.texelSize,
			                   rotatedCascade.texelSize,
			                   tolerance),
			       "camera rotation changed the cascade texel size");

			const glm::vec2 baseUv =
				shadowUv(baseCascade.worldToShadowTexture, baseCascade.receiverCenter);
			const glm::vec2 rotatedUv =
				shadowUv(rotatedCascade.worldToShadowTexture, baseCascade.receiverCenter);
			const glm::vec2 texelDelta =
				(rotatedUv - baseUv) * static_cast<float>(resolution);
			expect(nearlyEqual(texelDelta.x, std::round(texelDelta.x), 3.0e-3f)
			       && nearlyEqual(texelDelta.y, std::round(texelDelta.y), 3.0e-3f),
			       "camera rotation moved the cascade by a fractional shadow texel");
		}
		verifyFixedWorldPointsMoveByWholeTexels(
			baseFrame,
			rotatedFrame,
			resolution,
			4.0e-3f,
			"camera rotation moved a fixed receiver or caster point by a fractional shadow texel");
	}

	void testOffCenterProjection()
	{
		const demo::clipspace::ProjectionConvention convention =
			demo::clipspace::getProjectionConvention(demo::clipspace::BackendConvention::vulkan);
		glm::mat4 projection = demo::clipspace::makePerspectiveProjection(
			kFieldOfViewRadians,
			kAspectRatio,
			kNearPlane,
			kFarPlane,
			convention);
		projection[2][0] += 0.18f;
		projection[2][1] -= 0.12f;

		const glm::vec3 lightDirection = glm::normalize(glm::vec3(0.0f, -0.9848078f, 0.1736482f));
		const glm::vec3 cameraPosition(-8.0f, 4.0f, 11.0f);
		const glm::vec3 baseForward = glm::normalize(glm::vec3(0.42f, -0.16f, -0.89f));
		const shaderio::CameraUniforms baseCamera =
			makeCameraWithProjection(cameraPosition, baseForward, projection);

		demo::CSMShadowResources csm;
		updateProductionCascades(csm, baseCamera, lightDirection);
		const demo::CSMShadowResources::FrameData baseFrame = csm.getFrameData();
		const uint32_t resolution = csm.getCascadeResolution();
		verifySingleTexelGrid(baseFrame, resolution);
		verifyCascadeBlendCoverage(baseCamera, baseFrame);
		verifyCasterDepthAndCulling(baseFrame);

		const glm::vec3 firstCenterView = glm::vec3(
			baseCamera.view * glm::vec4(baseFrame.cascades[0].receiverCenter, 1.0f));
		expect(std::abs(firstCenterView.x) > 0.01f || std::abs(firstCenterView.y) > 0.01f,
		       "off-center projection shift was ignored when fitting the cascade");

		const shaderio::CameraUniforms rotatedCamera = makeCameraWithProjection(
			cameraPosition,
			glm::normalize(glm::vec3(-0.28f, -0.24f, -0.93f)),
			projection);
		updateProductionCascades(csm, rotatedCamera, lightDirection);
		const demo::CSMShadowResources::FrameData rotatedFrame = csm.getFrameData();
		verifySingleTexelGrid(rotatedFrame, resolution);
		verifyCascadeBlendCoverage(rotatedCamera, rotatedFrame);
		verifyCasterDepthAndCulling(rotatedFrame);

		for (uint32_t cascadeIndex = 0; cascadeIndex < baseFrame.cascadeCount; ++cascadeIndex)
		{
			const auto& baseCascade = baseFrame.cascades[cascadeIndex];
			const auto& rotatedCascade = rotatedFrame.cascades[cascadeIndex];
			const float tolerance = std::max(baseCascade.texelSize * 1.0e-5f, 1.0e-6f);
			expect(nearlyEqual(baseCascade.texelSize, rotatedCascade.texelSize, tolerance),
			       "off-center projection rotation changed the cascade texel size");

			const glm::vec2 baseUv =
				shadowUv(baseCascade.worldToShadowTexture, baseCascade.receiverCenter);
			const glm::vec2 rotatedUv =
				shadowUv(rotatedCascade.worldToShadowTexture, baseCascade.receiverCenter);
			const glm::vec2 texelDelta =
				(rotatedUv - baseUv) * static_cast<float>(resolution);
			expect(nearlyEqual(texelDelta.x, std::round(texelDelta.x), 3.0e-3f)
			       && nearlyEqual(texelDelta.y, std::round(texelDelta.y), 3.0e-3f),
			       "off-center projection rotation moved the cascade by a fractional shadow texel");
		}
		verifyFixedWorldPointsMoveByWholeTexels(
			baseFrame,
			rotatedFrame,
			resolution,
			4.0e-3f,
			"off-center projection rotation moved a fixed point by a fractional shadow texel");
	}

	void testOffCenterOrthographicProjection()
	{
		const demo::clipspace::ProjectionConvention convention =
			demo::clipspace::getProjectionConvention(demo::clipspace::BackendConvention::vulkan);
		const glm::mat4 projection = demo::clipspace::makeOrthographicProjection(
			-3.0f, 5.0f, -2.0f, 4.0f, kNearPlane, 150.0f, convention);
		const glm::vec3 lightDirection = glm::normalize(glm::vec3(0.0f, -0.9848078f, 0.1736482f));
		const glm::vec3 cameraPosition(3.0f, 7.0f, 9.0f);
		const shaderio::CameraUniforms baseCamera = makeCameraWithProjection(
			cameraPosition,
			glm::normalize(glm::vec3(-0.35f, -0.20f, -0.92f)),
			projection);

		demo::CSMShadowResources csm;
		updateProductionCascades(csm, baseCamera, lightDirection);
		const demo::CSMShadowResources::FrameData baseFrame = csm.getFrameData();
		const uint32_t resolution = csm.getCascadeResolution();
		verifySingleTexelGrid(baseFrame, resolution);
		verifyCascadeBlendCoverage(baseCamera, baseFrame);
		verifyCasterDepthAndCulling(baseFrame);

		const glm::vec3 firstCenterView = glm::vec3(
			baseCamera.view * glm::vec4(baseFrame.cascades[0].receiverCenter, 1.0f));
		expect(std::abs(firstCenterView.x) > 0.01f || std::abs(firstCenterView.y) > 0.01f,
		       "off-center orthographic projection shift was ignored");

		const shaderio::CameraUniforms rotatedCamera = makeCameraWithProjection(
			cameraPosition,
			glm::normalize(glm::vec3(0.31f, -0.27f, -0.91f)),
			projection);
		updateProductionCascades(csm, rotatedCamera, lightDirection);
		const demo::CSMShadowResources::FrameData rotatedFrame = csm.getFrameData();
		verifySingleTexelGrid(rotatedFrame, resolution);
		verifyCascadeBlendCoverage(rotatedCamera, rotatedFrame);
		verifyCasterDepthAndCulling(rotatedFrame);

		for (uint32_t cascadeIndex = 0; cascadeIndex < baseFrame.cascadeCount; ++cascadeIndex)
		{
			const auto& baseCascade = baseFrame.cascades[cascadeIndex];
			const auto& rotatedCascade = rotatedFrame.cascades[cascadeIndex];
			const float tolerance = std::max(baseCascade.texelSize * 1.0e-5f, 1.0e-6f);
			expect(nearlyEqual(baseCascade.texelSize, rotatedCascade.texelSize, tolerance),
			       "off-center orthographic rotation changed the cascade texel size");

			const glm::vec2 baseUv =
				shadowUv(baseCascade.worldToShadowTexture, baseCascade.receiverCenter);
			const glm::vec2 rotatedUv =
				shadowUv(rotatedCascade.worldToShadowTexture, baseCascade.receiverCenter);
			const glm::vec2 texelDelta =
				(rotatedUv - baseUv) * static_cast<float>(resolution);
			expect(nearlyEqual(texelDelta.x, std::round(texelDelta.x), 3.0e-3f)
			       && nearlyEqual(texelDelta.y, std::round(texelDelta.y), 3.0e-3f),
			       "off-center orthographic rotation moved the cascade by a fractional shadow texel");
		}
		verifyFixedWorldPointsMoveByWholeTexels(
			baseFrame,
			rotatedFrame,
			resolution,
			4.0e-3f,
			"off-center orthographic rotation moved a fixed point by a fractional shadow texel");
	}

	void testLastMovingToSettledTemporalSequence()
	{
		const glm::vec3 lightDirection = glm::normalize(glm::vec3(0.0f, -0.9848078f, 0.1736482f));
		const glm::vec3 finalPosition(10.36013f, 5.722083f, 4.936913f);
		const glm::vec3 finalForward = glm::normalize(glm::vec3(-0.62f, -0.18f, -0.76f));
		const shaderio::CameraUniforms previousMovingCamera = makeCamera(
			finalPosition + glm::vec3(-0.075f, 0.018f, 0.052f),
			glm::normalize(finalForward + glm::vec3(-0.012f, 0.006f, 0.008f)));
		shaderio::CameraUniforms previousMovingJittered = previousMovingCamera;
		applyTemporalProjectionJitter(previousMovingJittered, haltonJitterNdc(5u));

		shaderio::CameraUniforms lastMovingCamera = makeCamera(finalPosition, finalForward);
		setPreviousCameraMatrices(lastMovingCamera, previousMovingCamera, previousMovingJittered.viewProjection);
		shaderio::CameraUniforms lastMovingJittered = lastMovingCamera;
		applyTemporalProjectionJitter(lastMovingJittered, haltonJitterNdc(6u));

		shaderio::CameraUniforms firstStillCamera = makeCamera(finalPosition, finalForward);
		setPreviousCameraMatrices(firstStillCamera, makeCamera(finalPosition, finalForward),
		                          lastMovingJittered.viewProjection);
		shaderio::CameraUniforms firstStillJittered = firstStillCamera;
		applyTemporalProjectionJitter(firstStillJittered, haltonJitterNdc(7u));

		shaderio::CameraUniforms settledCamera = makeCamera(finalPosition, finalForward);
		setPreviousCameraMatrices(settledCamera, makeCamera(finalPosition, finalForward),
		                          firstStillJittered.viewProjection);

		expect(matricesNearlyEqual(lastMovingCamera.view, firstStillCamera.view, 0.0f)
		       && matricesNearlyEqual(lastMovingCamera.projection, firstStillCamera.projection, 0.0f),
		       "last-moving and first-still current cameras are not identical");
		expect(!matricesNearlyEqual(lastMovingCamera.prevViewProjection,
		                            firstStillCamera.prevViewProjection, 1.0e-5f),
		       "last-moving and first-still previous matrices are not deliberately different");

		demo::CSMShadowResources csm;
		const auto lastMovingFrame = prepareTemporalFrameCascades(
			csm, lastMovingCamera, lightDirection, haltonJitterNdc(6u));
		const auto firstStillFrame = prepareTemporalFrameCascades(
			csm, firstStillCamera, lightDirection, haltonJitterNdc(7u));
		const auto settledFrame = prepareTemporalFrameCascades(
			csm, settledCamera, lightDirection, haltonJitterNdc(8u));
		const uint32_t resolution = csm.getCascadeResolution();

		verifyCompleteCascadeFramesEqual(lastMovingFrame, firstStillFrame,
		                                 "last-moving to first-still CSM result depended on previous matrices");
		verifyCompleteCascadeFramesEqual(firstStillFrame, settledFrame,
		                                 "first-still to settled CSM result changed");
		verifyFixedWorldAnchorsMoveByWholeTexels(lastMovingFrame, firstStillFrame, resolution, 2.0e-3f,
		                                         "a fixed anchor moved fractionally at first-still");
		verifyFixedWorldAnchorsMoveByWholeTexels(firstStillFrame, settledFrame, resolution, 2.0e-3f,
		                                         "a fixed anchor moved fractionally while settling");

		demo::CSMShadowResources referenceCsm;
		updateProductionCascades(referenceCsm, makeCamera(finalPosition, finalForward), lightDirection);
		verifyCompleteCascadeFramesEqual(referenceCsm.getFrameData(), settledFrame,
		                                 "CSM result did not depend solely on the current unjittered camera");
	}

	void testManySmallTemporalMotionSteps()
	{
		constexpr uint32_t kStepCount = 32u;
		const glm::vec3 lightDirection = glm::normalize(glm::vec3(0.0f, -0.9848078f, 0.1736482f));
		const glm::vec3 startPosition(-3.25f, 6.5f, 12.0f);
		const glm::vec3 startForward = glm::normalize(glm::vec3(0.31f, -0.19f, -0.93f));
		shaderio::CameraUniforms previousCamera = makeCamera(
			startPosition + glm::vec3(-0.025f, -0.004f, 0.015f),
			glm::normalize(startForward + glm::vec3(-0.002f, 0.001f, 0.001f)));
		shaderio::CameraUniforms previousJitteredCamera = previousCamera;
		applyTemporalProjectionJitter(previousJitteredCamera, haltonJitterNdc(0u));
		glm::mat4 previousJitteredViewProjection = previousJitteredCamera.viewProjection;

		demo::CSMShadowResources sequenceCsm;
		demo::CSMShadowResources referenceCsm;
		demo::CSMShadowResources::FrameData previousFrame{};
		bool previousFrameValid = false;

		for (uint32_t step = 0; step < kStepCount; ++step)
		{
			const float s = static_cast<float>(step);
			const glm::vec3 position = startPosition + glm::vec3(0.025f * s, 0.004f * s, -0.015f * s);
			glm::mat4 rotation(1.0f);
			rotation = glm::rotate(rotation, glm::radians(0.08f * s), glm::vec3(0.0f, 1.0f, 0.0f));
			rotation = glm::rotate(rotation, glm::radians(0.035f * s), glm::vec3(1.0f, 0.0f, 0.0f));
			const glm::vec3 forward = glm::normalize(glm::vec3(rotation * glm::vec4(startForward, 0.0f)));

			shaderio::CameraUniforms currentCamera = makeCamera(position, forward);
			setPreviousCameraMatrices(currentCamera, previousCamera, previousJitteredViewProjection);
			const auto currentFrame = prepareTemporalFrameCascades(
				sequenceCsm, currentCamera, lightDirection, haltonJitterNdc(step + 1u));

			updateProductionCascades(referenceCsm, makeCamera(position, forward), lightDirection);
			verifyCompleteCascadeFramesEqual(referenceCsm.getFrameData(), currentFrame,
			                                 "small-motion CSM depended on prev fields or Halton jitter");
			if (previousFrameValid)
			{
				verifyFixedWorldAnchorsMoveByWholeTexels(
					previousFrame, currentFrame, sequenceCsm.getCascadeResolution(), 5.0e-3f,
					"small motion moved a fixed world anchor by a fractional shadow texel");
			}

			shaderio::CameraUniforms currentJitteredCamera = currentCamera;
			applyTemporalProjectionJitter(currentJitteredCamera, haltonJitterNdc(step + 1u));
			previousCamera = currentCamera;
			previousJitteredViewProjection = currentJitteredCamera.viewProjection;
			previousFrame = currentFrame;
			previousFrameValid = true;
		}
	}

	void verifyPcfGuardAndPublishedMetrics(
		const demo::CSMShadowResources& csm,
		const demo::CSMShadowResources::FrameData& frameData)
	{
		expect(shaderio::LCascadePcfGuardTexels >= shaderio::LCascadePcfRadius + 1,
		       "PCF guard does not cover filter radius plus center-snap phase");
		const float resolution = static_cast<float>(csm.getCascadeResolution());
		const float tapRadiusUv = static_cast<float>(shaderio::LCascadePcfRadius) / resolution;
		const shaderio::ShadowUniforms* shadow = csm.getShadowUniformsData();
		expect(shadow != nullptr, "shadow uniforms are unavailable");

		for (uint32_t cascadeIndex = 0; cascadeIndex < frameData.cascadeCount; ++cascadeIndex)
		{
			const auto& cascade = frameData.cascades[cascadeIndex];
			expect(nearlyEqual(cascade.depthRange, cascade.farPlane - cascade.nearPlane, 1.0e-5f),
			       "published cascade depth range does not match near/far");
			expect(nearlyEqual(cascade.invDepthRange, 1.0f / cascade.depthRange, 1.0e-7f),
			       "cascade inverse depth range is incorrect");
			expect(nearlyEqual(shadow->cascadeSplitDistances.invDepthRange[cascadeIndex],
			                   cascade.invDepthRange, 1.0e-7f),
			       "ShadowUniforms did not publish inverse depth range");
			expect(nearlyEqual(shadow->cascadeSplitDistances.worldTexelSize[cascadeIndex],
			                   cascade.texelSize, 1.0e-7f),
			       "ShadowUniforms did not publish world texel size");

			for (const glm::vec3& corner : cascade.receiverCornersWorld)
			{
				const glm::vec2 uv = shadowUv(cascade.worldToShadowTexture, corner);
				const float epsilon = 2.0e-5f;
				expect(uv.x - tapRadiusUv >= -epsilon && uv.x + tapRadiusUv <= 1.0f + epsilon
				       && uv.y - tapRadiusUv >= -epsilon && uv.y + tapRadiusUv <= 1.0f + epsilon,
				       "receiver PCF footprint escapes the guarded cascade projection");
			}
		}
	}

	void verifyDepthSpansStable(
		const demo::CSMShadowResources::FrameData& expected,
		const demo::CSMShadowResources::FrameData& actual,
		const char* message)
	{
		expect(expected.cascadeCount == actual.cascadeCount, message);
		for (uint32_t cascadeIndex = 0; cascadeIndex < expected.cascadeCount; ++cascadeIndex)
		{
			const auto& a = expected.cascades[cascadeIndex];
			const auto& b = actual.cascades[cascadeIndex];
			expect(nearlyEqual(a.nearPlane, b.nearPlane, 1.0e-6f), message);
			expect(nearlyEqual(a.depthRange, b.depthRange, 1.0e-5f), message);
			expect(nearlyEqual(a.invDepthRange, b.invDepthRange, 1.0e-7f), message);
			expect(nearlyEqual(a.farPlane, b.farPlane, 1.0e-5f), message);
		}
	}

	void testStableDepthRangeBiasAndGuard()
	{
		const glm::vec3 lightDirection = glm::normalize(glm::vec3(0.0f, -0.9848078f, 0.1736482f));
		const shaderio::CameraUniforms baseCamera = makeCamera(
			glm::vec3(-4.0f, 6.0f, 13.0f), glm::normalize(glm::vec3(0.28f, -0.20f, -0.94f)));
		const shaderio::CameraUniforms movedCamera = makeCamera(
			glm::vec3(8.5f, 9.0f, -3.0f), glm::normalize(glm::vec3(-0.61f, -0.12f, -0.78f)));

		demo::CSMShadowResources casterCsm;
		updateProductionCascades(casterCsm, baseCamera, lightDirection);
		const auto casterBase = casterCsm.getFrameData();
		verifyPcfGuardAndPublishedMetrics(casterCsm, casterBase);
		updateProductionCascades(casterCsm, movedCamera, lightDirection);
		const auto casterMoved = casterCsm.getFrameData();
		verifyPcfGuardAndPublishedMetrics(casterCsm, casterMoved);
		verifyDepthSpansStable(casterBase, casterMoved,
		                       "static caster bounds changed cascade depth span during camera motion");

		demo::CSMShadowResources fallbackCsm;
		fallbackCsm.updateCascadeMatrices(baseCamera, lightDirection, kShadowDistance,
		                                  glm::vec3(0.0f), glm::vec3(0.0f), false);
		const auto fallbackBase = fallbackCsm.getFrameData();
		verifyPcfGuardAndPublishedMetrics(fallbackCsm, fallbackBase);
		fallbackCsm.updateCascadeMatrices(movedCamera, lightDirection, kShadowDistance,
		                                  glm::vec3(0.0f), glm::vec3(0.0f), false);
		const auto fallbackMoved = fallbackCsm.getFrameData();
		verifyPcfGuardAndPublishedMetrics(fallbackCsm, fallbackMoved);
		verifyDepthSpansStable(fallbackBase, fallbackMoved,
		                       "receiver-sphere fallback changed depth span during camera motion");

		const shaderio::ShadowUniforms* shadow = casterCsm.getShadowUniformsData();
		shaderio::LightParams copiedLight{};
		copiedLight.cascadeSplitDistances = shadow->cascadeSplitDistances;
		const float receiverBiasWorld = 0.0015f;
		for (uint32_t cascadeIndex = 0; cascadeIndex < casterMoved.cascadeCount; ++cascadeIndex)
		{
			const auto& cascade = casterMoved.cascades[cascadeIndex];
			const float copiedInvDepthRange = copiedLight.cascadeSplitDistances.invDepthRange[cascadeIndex];
			expect(nearlyEqual(copiedInvDepthRange, cascade.invDepthRange, 1.0e-7f),
			       "LightParams compatibility copy dropped inverse depth range");
			const float biasNdc = receiverBiasWorld * copiedInvDepthRange;
			expect(nearlyEqual(biasNdc * cascade.depthRange, receiverBiasWorld, 1.0e-7f),
			       "receiver bias is not expressed in consistent world units across cascades");

			const float casterDepth = cascade.nearPlane + cascade.depthRange * 0.5f;
			const float storedDepth = (cascade.farPlane - casterDepth) * copiedInvDepthRange;
			const float withinBiasDepth = (cascade.farPlane - (casterDepth + receiverBiasWorld * 0.5f))
				                              * copiedInvDepthRange;
			const float beyondBiasDepth = (cascade.farPlane - (casterDepth + receiverBiasWorld * 2.0f))
				                              * copiedInvDepthRange;
			expect(withinBiasDepth + biasNdc >= storedDepth,
			       "reverse-Z receiver bias rejected a receiver inside the world-space tolerance");
			expect(beyondBiasDepth + biasNdc < storedDepth,
			       "reverse-Z receiver bias accepted a receiver beyond the world-space tolerance");
		}
	}

	void testUnjitteredShadowFitCameraConstruction()
	{
		const glm::vec3 cameraPosition(7.0f, 4.0f, 11.0f);
		const glm::vec3 cameraForward = glm::normalize(glm::vec3(-0.44f, -0.16f, -0.88f));
		const shaderio::CameraUniforms perspectiveCamera =
			makeCamera(cameraPosition, cameraForward);
		shaderio::CameraUniforms jitteredPerspective = perspectiveCamera;
		applyTemporalProjectionJitter(jitteredPerspective, glm::vec2(0.006f, -0.003f));
		// The helper reconstructs this field from the stable VP instead of trusting an
		// optional inverse that may be absent in a legacy caller.
		jitteredPerspective.unjitteredInverseViewProjection = glm::mat4(0.0f);

		const shaderio::CameraUniforms perspectiveShadowFit =
			demo::makeUnjitteredShadowFitCamera(jitteredPerspective);
		expect(matricesNearlyEqual(perspectiveShadowFit.projection,
		                           perspectiveCamera.projection, 2.0e-5f),
		       "perspective shadow-fit camera retained temporal projection jitter");
		expect(matricesNearlyEqual(perspectiveShadowFit.viewProjection,
		                           perspectiveCamera.viewProjection, 2.0e-5f),
		       "perspective shadow-fit camera did not select unjittered VP");
		expect(matricesNearlyEqual(perspectiveShadowFit.inverseViewProjection,
		                           perspectiveCamera.inverseViewProjection, 2.0e-4f),
		       "perspective shadow-fit inverse VP was not reconstructed");

		shaderio::CameraUniforms zeroTemporalField = jitteredPerspective;
		zeroTemporalField.unjitteredViewProjection = glm::mat4(0.0f);
		const shaderio::CameraUniforms zeroFallback =
			demo::makeUnjitteredShadowFitCamera(zeroTemporalField);
		expect(matricesNearlyEqual(zeroFallback.projection,
		                           jitteredPerspective.projection, 2.0e-5f),
		       "zero unjittered VP did not fall back to the coherent current projection");

		shaderio::CameraUniforms identityTemporalField = jitteredPerspective;
		identityTemporalField.unjitteredViewProjection = glm::mat4(1.0f);
		const shaderio::CameraUniforms identityFallback =
			demo::makeUnjitteredShadowFitCamera(identityTemporalField);
		expect(matricesNearlyEqual(identityFallback.projection,
		                           jitteredPerspective.projection, 2.0e-5f),
		       "identity temporal sentinel was consumed as a real camera VP");

		const demo::clipspace::ProjectionConvention convention =
			demo::clipspace::getProjectionConvention(demo::clipspace::BackendConvention::vulkan);
		const glm::mat4 orthographicProjection = demo::clipspace::makeOrthographicProjection(
			-6.0f, 4.0f, -3.0f, 5.0f, kNearPlane, 180.0f, convention);
		const shaderio::CameraUniforms orthographicCamera = makeCameraWithProjection(
			glm::vec3(2.0f, 8.0f, 6.0f),
			glm::normalize(glm::vec3(-0.25f, -0.31f, -0.92f)),
			orthographicProjection);
		shaderio::CameraUniforms jitteredOrthographic = orthographicCamera;
		applyTemporalProjectionJitter(jitteredOrthographic, glm::vec2(-0.004f, 0.005f));

		const shaderio::CameraUniforms orthographicShadowFit =
			demo::makeUnjitteredShadowFitCamera(jitteredOrthographic);
		expect(matricesNearlyEqual(orthographicShadowFit.projection,
		                           orthographicCamera.projection, 2.0e-5f),
		       "orthographic shadow-fit camera retained temporal projection jitter");
		expect(matricesNearlyEqual(orthographicShadowFit.viewProjection,
		                           orthographicCamera.viewProjection, 2.0e-5f),
		       "orthographic shadow-fit camera did not select unjittered VP");
	}

	void testPreparedCascadesSurviveTemporalJitter()
	{
		const glm::vec3 lightDirection = glm::normalize(glm::vec3(0.0f, -0.9848078f, 0.1736482f));
		const glm::vec3 cameraPosition(10.36013f, 5.722083f, 4.936913f);
		const glm::vec3 cameraForward = glm::normalize(glm::vec3(-0.62f, -0.18f, -0.76f));

		demo::CSMShadowResources csm;
		const shaderio::CameraUniforms baseCamera = makeCamera(cameraPosition, cameraForward);
		const shaderio::CameraUniforms baseShadowFitCamera =
			demo::makeUnjitteredShadowFitCamera(baseCamera);
		updateProductionCascades(csm, baseShadowFitCamera, lightDirection);
		const demo::CSMShadowResources::FrameData baseFrame = csm.getFrameData();

		shaderio::CameraUniforms jitteredCamera = baseCamera;
		applyTemporalProjectionJitter(jitteredCamera, glm::vec2(0.005f, -0.004f));
		demo::RenderParams gpuDrivenParams{};
		gpuDrivenParams.cameraUniforms = &jitteredCamera;
		gpuDrivenParams.csmCascadeMatricesPrepared = true;
		expect(!demo::shouldUpdateCSMCascadeMatrices(gpuDrivenParams),
		       "a GPU-driven frame attempted to overwrite its prepared CSM snapshot");
		if (demo::shouldUpdateCSMCascadeMatrices(gpuDrivenParams))
		{
			updateProductionCascades(csm, jitteredCamera, lightDirection);
		}
		verifyCascadeFramesEqual(baseFrame, csm.getFrameData());

		demo::RenderParams directParams{};
		directParams.cameraUniforms = &jitteredCamera;
		expect(demo::shouldUpdateCSMCascadeMatrices(directParams),
		       "a direct RenderDevice frame no longer prepares its own CSM cascades");

		const shaderio::CameraUniforms directShadowFitCamera =
			demo::makeUnjitteredShadowFitCamera(jitteredCamera);
		updateProductionCascades(csm, directShadowFitCamera, lightDirection);
		verifyCascadeFramesEqual(baseFrame, csm.getFrameData());

		const demo::clipspace::ProjectionConvention convention =
			demo::clipspace::getProjectionConvention(demo::clipspace::BackendConvention::vulkan);
		const glm::mat4 orthographicProjection = demo::clipspace::makeOrthographicProjection(
			-3.0f, 5.0f, -2.0f, 4.0f, kNearPlane, 150.0f, convention);
		const shaderio::CameraUniforms orthographicCamera = makeCameraWithProjection(
			glm::vec3(3.0f, 7.0f, 9.0f),
			glm::normalize(glm::vec3(-0.35f, -0.20f, -0.92f)),
			orthographicProjection);
		const shaderio::CameraUniforms orthographicShadowFitCamera =
			demo::makeUnjitteredShadowFitCamera(orthographicCamera);
		updateProductionCascades(csm, orthographicShadowFitCamera, lightDirection);
		const demo::CSMShadowResources::FrameData orthographicFrame = csm.getFrameData();

		shaderio::CameraUniforms jitteredOrthographicCamera = orthographicCamera;
		applyTemporalProjectionJitter(jitteredOrthographicCamera, glm::vec2(-0.003f, 0.002f));
		gpuDrivenParams.cameraUniforms = &jitteredOrthographicCamera;
		if (demo::shouldUpdateCSMCascadeMatrices(gpuDrivenParams))
		{
			updateProductionCascades(csm, jitteredOrthographicCamera, lightDirection);
		}
		verifyCascadeFramesEqual(orthographicFrame, csm.getFrameData());

		const shaderio::CameraUniforms directOrthographicShadowFit =
			demo::makeUnjitteredShadowFitCamera(jitteredOrthographicCamera);
		updateProductionCascades(csm, directOrthographicShadowFit, lightDirection);
		verifyCascadeFramesEqual(orthographicFrame, csm.getFrameData());
	}
	void testCasterNormalOffsetDirectionAndMagnitude()
	{
		const glm::vec3 dirToLight(0.0f, 1.0f, 0.0f);
		const glm::vec3 facingNormal = glm::normalize(glm::vec3(0.6f, 0.8f, 0.0f));
		const glm::vec3 opposingNormal = -facingNormal;
		const float normalBiasWorld = 0.25f;
		const auto computeOffset = [&](const glm::vec3& worldNormal)
		{
			const float normalDotLight = glm::dot(worldNormal, dirToLight);
			const float cosTheta = std::clamp(std::abs(normalDotLight), 0.0f, 1.0f);
			const float sinTheta = std::sqrt(std::max(1.0f - cosTheta * cosTheta, 0.0f));
			const glm::vec3 offsetNormal = normalDotLight >= 0.0f ? -worldNormal : worldNormal;
			return offsetNormal * (normalBiasWorld * sinTheta);
		};

		const glm::vec3 facingOffset = computeOffset(facingNormal);
		const glm::vec3 opposingOffset = computeOffset(opposingNormal);
		const float expectedMagnitude = normalBiasWorld * 0.6f;
		expect(glm::dot(facingOffset, dirToLight) < 0.0f,
		       "front-facing caster normal offset moved toward the light source");
		expect(glm::dot(opposingOffset, dirToLight) < 0.0f,
		       "back-facing caster normal offset moved toward the light source");
		expect(glm::dot(glm::normalize(facingOffset), -facingNormal) > 0.9999f,
		       "front-facing caster did not select -normal for its offset");
		expect(glm::dot(glm::normalize(opposingOffset), opposingNormal) > 0.9999f,
		       "back-facing caster did not select +normal for its offset");
		expect(nearlyEqual(glm::length(facingOffset), expectedMagnitude, 1.0e-6f)
		       && nearlyEqual(glm::length(opposingOffset), expectedMagnitude, 1.0e-6f),
		       "caster normal offset did not preserve normalBias * sinTheta world-space magnitude");
	}

	void testInverseTransposeNormalMatrix()
	{
		const glm::mat4 model = glm::rotate(
			glm::mat4(1.0f), glm::radians(37.0f), glm::normalize(glm::vec3(0.2f, 0.7f, 0.4f)))
			* glm::scale(glm::mat4(1.0f), glm::vec3(-2.0f, 0.5f, 3.0f));
		const glm::mat3 modelLinear(model);
		const glm::vec3 localNormal = glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f));
		const glm::vec3 localTangent = glm::normalize(glm::vec3(1.0f, -1.0f, 0.0f));
		const glm::vec3 worldNormal = glm::normalize(
			glm::transpose(glm::inverse(modelLinear)) * localNormal);
		const glm::vec3 worldTangent = glm::normalize(modelLinear * localTangent);
		const glm::vec3 incorrectlyTransformedNormal = glm::normalize(modelLinear * localNormal);
		const auto element = [&](uint32_t row, uint32_t column) { return modelLinear[column][row]; };
		const float cofactor00 = element(1u, 1u) * element(2u, 2u) - element(1u, 2u) * element(2u, 1u);
		const float cofactor01 = element(1u, 2u) * element(2u, 0u) - element(1u, 0u) * element(2u, 2u);
		const float cofactor02 = element(1u, 0u) * element(2u, 1u) - element(1u, 1u) * element(2u, 0u);
		const float cofactor10 = element(0u, 2u) * element(2u, 1u) - element(0u, 1u) * element(2u, 2u);
		const float cofactor11 = element(0u, 0u) * element(2u, 2u) - element(0u, 2u) * element(2u, 0u);
		const float cofactor12 = element(0u, 1u) * element(2u, 0u) - element(0u, 0u) * element(2u, 1u);
		const float cofactor20 = element(0u, 1u) * element(1u, 2u) - element(0u, 2u) * element(1u, 1u);
		const float cofactor21 = element(0u, 2u) * element(1u, 0u) - element(0u, 0u) * element(1u, 2u);
		const float cofactor22 = element(0u, 0u) * element(1u, 1u) - element(0u, 1u) * element(1u, 0u);
		const float determinantValue = element(0u, 0u) * cofactor00
			+ element(0u, 1u) * cofactor01 + element(0u, 2u) * cofactor02;
		const glm::vec3 cofactorNormal = glm::normalize(glm::vec3(
			cofactor00 * localNormal.x + cofactor01 * localNormal.y + cofactor02 * localNormal.z,
			cofactor10 * localNormal.x + cofactor11 * localNormal.y + cofactor12 * localNormal.z,
			cofactor20 * localNormal.x + cofactor21 * localNormal.y + cofactor22 * localNormal.z)
			/ determinantValue);

		expect(vectorsNearlyEqual(cofactorNormal, worldNormal, 1.0e-5f),
		       "shader cofactor helper does not match the model 3x3 inverse-transpose");
		expect(std::abs(glm::dot(worldNormal, worldTangent)) <= 1.0e-5f,
		       "inverse-transpose normal is not perpendicular after non-uniform scaling");
		expect(std::abs(glm::dot(incorrectlyTransformedNormal, worldTangent)) > 0.1f,
		       "normal-matrix regression fixture does not distinguish direct model 3x3 multiplication");
	}

	void testStableLightBasisAcrossVerticalThreshold()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		constexpr float kAzimuth = 0.61f;
		constexpr float kBelowThreshold = 0.9499f;
		constexpr float kAboveThreshold = 0.9501f;

		for (const float verticalSign : {-1.0f, 1.0f})
		{
			const glm::vec3 beforeDirection =
				directionWithVerticalComponent(verticalSign * kBelowThreshold, kAzimuth) * 17.0f;
			const glm::vec3 afterDirection =
				directionWithVerticalComponent(verticalSign * kAboveThreshold, kAzimuth) * 0.125f;

			demo::CSMShadowResources csm;
			csm.updateCascadeMatrices(camera, beforeDirection, kShadowDistance);
			const auto beforeFrame = csm.getFrameData();
			csm.updateCascadeMatrices(camera, afterDirection, kShadowDistance);
			const auto afterFrame = csm.getFrameData();

			expect(nearlyEqual(glm::length(beforeFrame.lightDirection), 1.0f, 1.0e-6f)
			       && nearlyEqual(glm::length(afterFrame.lightDirection), 1.0f, 1.0e-6f),
			       "near-vertical light directions were not normalized");
			for (uint32_t cascadeIndex = 0; cascadeIndex < beforeFrame.cascadeCount; ++cascadeIndex)
			{
				verifyLightFrameAxes(beforeFrame.cascades[cascadeIndex].lightView);
				verifyLightFrameAxes(afterFrame.cascades[cascadeIndex].lightView);
				const LightFrameAxes beforeAxes =
					extractLightFrameAxes(beforeFrame.cascades[cascadeIndex].lightView);
				const LightFrameAxes afterAxes =
					extractLightFrameAxes(afterFrame.cascades[cascadeIndex].lightView);
				expect(glm::dot(beforeAxes.right, afterAxes.right) > 0.999f
				       && glm::dot(beforeAxes.up, afterAxes.up) > 0.999f,
				       "light-space grid rolled discontinuously across abs(y)=0.95");
			}
		}

		for (const glm::vec3 singularDirection : {
			     glm::vec3(0.0f, 0.0f, -4.0f),
			     glm::vec3(1.0e-5f, -2.0e-5f, -1.0f) * 9.0f})
		{
			demo::CSMShadowResources csm;
			csm.updateCascadeMatrices(camera, singularDirection, kShadowDistance);
			verifyLightFrameAxes(csm.getFrameData().cascades[0].lightView);
		}
	}

	void testReceiverDrivenCascadeCullingFootprint()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(6.0f, 8.0f, 14.0f), glm::normalize(glm::vec3(-0.32f, -0.21f, -0.92f)));
		const glm::vec3 verticalLightDirection(0.0f, -7.0f, 0.0f);

		demo::CSMShadowResources compactCsm;
		compactCsm.updateCascadeMatrices(
			camera, verticalLightDirection, kShadowDistance,
			glm::vec3(-20.0f, -2.0f, -20.0f), glm::vec3(20.0f, 18.0f, 20.0f), true);
		const auto compactFrame = compactCsm.getFrameData();

		demo::CSMShadowResources wideCsm;
		wideCsm.updateCascadeMatrices(
			camera, verticalLightDirection, kShadowDistance,
			glm::vec3(-5000.0f, -2.0f, -5000.0f), glm::vec3(5000.0f, 18.0f, 5000.0f), true);
		const auto wideFrame = wideCsm.getFrameData();

		demo::CSMShadowResources tallCsm;
		tallCsm.updateCascadeMatrices(
			camera, verticalLightDirection, kShadowDistance,
			glm::vec3(-20.0f, -200.0f, -20.0f), glm::vec3(20.0f, 400.0f, 20.0f), true);
		const auto tallFrame = tallCsm.getFrameData();

		for (uint32_t cascadeIndex = 0; cascadeIndex < compactFrame.cascadeCount; ++cascadeIndex)
		{
			const auto& compactCascade = compactFrame.cascades[cascadeIndex];
			const auto& wideCascade = wideFrame.cascades[cascadeIndex];
			const auto& tallCascade = tallFrame.cascades[cascadeIndex];
			for (uint32_t planeIndex = 0; planeIndex < 4u; ++planeIndex)
			{
				expect(vectorsNearlyEqual(compactCascade.cullingPlanes[planeIndex],
				                          wideCascade.cullingPlanes[planeIndex], 1.0e-5f),
				       "full-scene caster XY bounds expanded a cascade culling footprint");
				expect(vectorsNearlyEqual(compactCascade.cullingPlanes[planeIndex],
				                          tallCascade.cullingPlanes[planeIndex], 1.0e-5f),
				       "caster Z bounds changed a cascade XY culling footprint");
			}

			expect(nearlyEqual(compactCascade.depthRange, wideCascade.depthRange, 1.0e-5f),
			       "caster XY extent changed the stable depth span");
			expect(tallCascade.depthRange > compactCascade.depthRange + 100.0f,
			       "full-scene caster Z bounds no longer stabilize the depth span");

			const float renderWidth = orthographicWidth(compactCascade.lightProjection);
			const float cullWidth =
				glm::dot(glm::vec3(compactCascade.cullingPlanes[0]), compactCascade.receiverCenter)
				+ compactCascade.cullingPlanes[0].w
				+ glm::dot(glm::vec3(compactCascade.cullingPlanes[1]), compactCascade.receiverCenter)
				+ compactCascade.cullingPlanes[1].w;
			expect(cullWidth < renderWidth + std::max(compactCascade.texelSize * 32.0f, 1.0f),
			       "per-cascade XY culling is no longer close to the receiver projection");

			const LightFrameAxes axes = extractLightFrameAxes(compactCascade.lightView);
			const glm::vec3 unrelatedCaster = compactCascade.receiverCenter
				+ axes.right * (renderWidth * 4.0f)
				+ axes.up * (renderWidth * 4.0f);
			bool rejectedBySidePlane = false;
			for (uint32_t planeIndex = 0; planeIndex < 4u; ++planeIndex)
			{
				const glm::vec4& plane = compactCascade.cullingPlanes[planeIndex];
				rejectedBySidePlane |= glm::dot(glm::vec3(plane), unrelatedCaster) + plane.w < 0.0f;
			}
			expect(rejectedBySidePlane,
			       "an unrelated far-XY caster was retained by a per-cascade culling footprint");
		}
	}

	void testPersistentLightBasisAcrossSouthPoleSeam()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		const std::array<glm::vec3, 9> seamDirections{
			glm::vec3(2.0e-2f, 0.0f, -1.0f),
			glm::vec3(5.0e-3f, 0.0f, -1.0f),
			glm::vec3(5.0e-4f, 0.0f, -1.0f),
			glm::vec3(1.0e-5f, 0.0f, -1.0f),
			glm::vec3(0.0f, 0.0f, -1.0f),
			glm::vec3(-1.0e-5f, 0.0f, -1.0f),
			glm::vec3(-5.0e-4f, 0.0f, -1.0f),
			glm::vec3(-5.0e-3f, 0.0f, -1.0f),
			glm::vec3(-2.0e-2f, 0.0f, -1.0f),
		};

		demo::CSMShadowResources csm;
		LightFrameAxes previousAxes{};
		bool havePreviousAxes = false;
		for (size_t directionIndex = 0; directionIndex < seamDirections.size(); ++directionIndex)
		{
			const float inputScale = directionIndex % 2u == 0u ? 13.0f : 0.125f;
			csm.updateCascadeMatrices(camera, seamDirections[directionIndex] * inputScale, kShadowDistance);
			const auto& frame = csm.getFrameData();
			expect(nearlyEqual(glm::length(frame.lightDirection), 1.0f, 1.0e-6f),
			       "south-pole seam input was not normalized");
			verifyLightFrameAxes(frame.cascades[0].lightView);
			const LightFrameAxes axes = extractLightFrameAxes(frame.cascades[0].lightView);
			if (havePreviousAxes)
			{
				expect(glm::dot(previousAxes.right, axes.right) > 0.999f
				       && glm::dot(previousAxes.up, axes.up) > 0.999f,
				       "persistent CSM light basis rolled across the -Z seam");
			}
			previousAxes = axes;
			havePreviousAxes = true;
		}

		csm.updateCascadeMatrices(
			camera, glm::vec3(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f), kShadowDistance);
		expect(vectorsNearlyEqual(csm.getFrameData().lightDirection, glm::vec3(0.0f, -1.0f, 0.0f), 1.0e-6f),
		       "invalid light direction did not reset to the safe fallback");
		verifyLightFrameAxes(csm.getFrameData().cascades[0].lightView);

		csm.updateCascadeMatrices(camera, glm::vec3(0.0f, 0.0f, -1.0f), kShadowDistance);
		csm.updateCascadeMatrices(camera, glm::vec3(0.0f, 0.0f, 1.0f), kShadowDistance);
		verifyLightFrameAxes(csm.getFrameData().cascades[0].lightView);
	}

	void testPersistentLightBasisAcrossAntipodalDirection()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		const glm::vec3 initialDirection =
			glm::normalize(glm::vec3(0.10f, -0.96f, -0.25f));
		const glm::vec3 transportedDirection =
			glm::normalize(glm::vec3(0.65f, -0.55f, -0.52f));

		demo::CSMShadowResources csm;
		csm.updateCascadeMatrices(camera, initialDirection * 11.0f, kShadowDistance);
		const LightFrameAxes initialAxes =
			extractLightFrameAxes(csm.getFrameData().cascades[0].lightView);
		csm.updateCascadeMatrices(camera, transportedDirection * 0.125f, kShadowDistance);
		const LightFrameAxes transportedAxes =
			extractLightFrameAxes(csm.getFrameData().cascades[0].lightView);

		verifyLightFrameAxes(csm.getFrameData().cascades[0].lightView);
		const glm::vec3 absoluteTransportedUp = glm::abs(transportedAxes.up);
		expect(glm::dot(initialAxes.up, transportedAxes.up) < 0.95f
		       && absoluteTransportedUp.x < 0.95f
		       && absoluteTransportedUp.y < 0.95f
		       && absoluteTransportedUp.z < 0.95f,
		       "antipodal regression fixture did not first transport history to a non-axis up");

		csm.updateCascadeMatrices(camera, -transportedDirection * 7.0f, kShadowDistance);
		const auto& antipodalFrame = csm.getFrameData();
		verifyLightFrameAxes(antipodalFrame.cascades[0].lightView);
		const LightFrameAxes antipodalAxes =
			extractLightFrameAxes(antipodalFrame.cascades[0].lightView);
		expect(glm::dot(transportedAxes.up, antipodalAxes.up) > 0.9999f,
		       "180-degree light direction edit discarded the transported tangent-plane roll");
		expect(glm::dot(transportedAxes.back, antipodalAxes.back) < -0.9999f,
		       "antipodal regression fixture did not reverse the light direction");
	}

	void testNearAntipodalBoundaryPairIsContinuous()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		const NearAntipodalBasisSample lowerSide =
			evaluateNearAntipodalBasis(camera, -0.999901f);
		const NearAntipodalBasisSample upperSide =
			evaluateNearAntipodalBasis(camera, -0.999899f);

		expect(lowerSide.realizedDirectionDot < -0.9999f
		       && upperSide.realizedDirectionDot > -0.9999f,
		       "near-antipodal boundary fixture did not straddle the former threshold");
		expect(glm::dot(lowerSide.axes.up, upperSide.axes.up) > 0.9999f,
		       "near-antipodal boundary changed light up discontinuously");
		expect(glm::dot(lowerSide.axes.right, upperSide.axes.right) > 0.9999f,
		       "near-antipodal boundary changed light right discontinuously");
	}

	void testLightBasisContinuouslyApproachesAntipode()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		const std::array<float, 13> directionDots{
			-0.98f,
			-0.99f,
			-0.995f,
			-0.999f,
			-0.9995f,
			-0.9998f,
			-0.99989f,
			-0.999899f,
			-0.9999f,
			-0.999901f,
			-0.99991f,
			-0.99999f,
			-1.0f,
		};
		constexpr float kMaxAdjacentBasisAngleCosine = 0.995f;
		LightFrameAxes previousAxes{};
		bool havePreviousAxes = false;

		// Re-prime every sample so the scanned variable is exactly oldDirection dot
		// newDirection, rather than the tiny dot change between already-updated samples.
		for (const float directionDot : directionDots)
		{
			const NearAntipodalBasisSample sample =
				evaluateNearAntipodalBasis(camera, directionDot);
			expect(glm::dot(sample.historyAxes.up, sample.axes.up) > 0.999f,
			       "approaching the antipode discarded the historical tangent-plane roll");
			if (havePreviousAxes)
			{
				expect(glm::dot(previousAxes.up, sample.axes.up) > kMaxAdjacentBasisAngleCosine,
				       "successive near-antipodal samples changed up too sharply");
				expect(glm::dot(previousAxes.right, sample.axes.right) > kMaxAdjacentBasisAngleCosine,
				       "successive near-antipodal samples changed right too sharply");
			}
			previousAxes = sample.axes;
			havePreviousAxes = true;
		}
	}

	void testLightBasisContinuousAcrossFormerAntipodalThresholdSweep()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		constexpr uint32_t kSweepSampleCount = 41u;
		constexpr float kSweepStartDot = -0.99988f;
		constexpr float kSweepStep = -1.0e-6f;
		constexpr float kMaxSweepStepAngleCosine = 0.9999f;
		LightFrameAxes previousAxes{};
		bool havePreviousAxes = false;

		for (uint32_t sampleIndex = 0; sampleIndex < kSweepSampleCount; ++sampleIndex)
		{
			const float directionDot =
				kSweepStartDot + static_cast<float>(sampleIndex) * kSweepStep;
			const NearAntipodalBasisSample sample =
				evaluateNearAntipodalBasis(camera, directionDot);
			if (havePreviousAxes)
			{
				expect(glm::dot(previousAxes.up, sample.axes.up) > kMaxSweepStepAngleCosine,
				       "former antipodal threshold sweep contains an up-vector jump");
				expect(glm::dot(previousAxes.right, sample.axes.right) > kMaxSweepStepAngleCosine,
				       "former antipodal threshold sweep contains a right-vector jump");
			}
			previousAxes = sample.axes;
			havePreviousAxes = true;
		}
	}

	void testProjectionReliabilityBoundaryIsContinuousInFloat32()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		constexpr float kFormerProjectionLengthSqThreshold = 1.0e-8f;
		const float lowerTangentMagnitude = std::nextafter(1.0e-4f, 0.0f);
		const float upperTangentMagnitude =
			std::nextafter(1.0e-4f, std::numeric_limits<float>::infinity());
		constexpr float kMaximumPairAngleRadians = 1.0e-3f;

		for (const bool primeProjectionChart : std::array<bool, 2>{false, true})
		{
			for (const float historyUpSign : std::array<float, 2>{-1.0f, 1.0f})
			{
				const ProjectionSingularityBasisSample lowerSide =
					evaluateProjectionSingularityBasis(
						camera,
						lowerTangentMagnitude,
						0.0f,
						historyUpSign,
						primeProjectionChart);
				const ProjectionSingularityBasisSample upperSide =
					evaluateProjectionSingularityBasis(
						camera,
						upperTangentMagnitude,
						0.0f,
						historyUpSign,
						primeProjectionChart);
				expect(lowerSide.realizedProjectedLengthSq < kFormerProjectionLengthSqThreshold
				       && upperSide.realizedProjectedLengthSq > kFormerProjectionLengthSqThreshold,
				       "float32 projection fixture did not straddle projectedLengthSq=1e-8");

				const float adjacentAngle = maxLightBasisAngleRadians(lowerSide.axes, upperSide.axes);
				g_maxProjectionSingularityAdjacentAngleRadians = std::max(
					g_maxProjectionSingularityAdjacentAngleRadians, adjacentAngle);
				expect(glm::dot(lowerSide.axes.up, upperSide.axes.up) > 0.99999f,
				       "projectedLengthSq=1e-8 float32 boundary flipped light up");
				expect(glm::dot(lowerSide.axes.right, upperSide.axes.right) > 0.99999f,
				       "projectedLengthSq=1e-8 float32 boundary flipped light right");
				expect(adjacentAngle < kMaximumPairAngleRadians,
				       "projectedLengthSq=1e-8 float32 boundary changed the persisted light basis");
			}
		}
	}

	void testDirectionDotZeroCrossingNearProjectionSingularityIsContinuous()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		const float projectionTangentMagnitude =
			std::nextafter(1.0e-4f, std::numeric_limits<float>::infinity());
		constexpr float kDirectionDotEpsilon = 1.0e-7f;
		constexpr float kMaximumPairAngleRadians = 5.0e-3f;

		for (const bool primeProjectionChart : std::array<bool, 2>{false, true})
		{
			for (const float historyUpSign : std::array<float, 2>{-1.0f, 1.0f})
			{
				const ProjectionSingularityBasisSample negativeSide =
					evaluateProjectionSingularityBasis(
						camera,
						projectionTangentMagnitude,
						-kDirectionDotEpsilon,
						historyUpSign,
						primeProjectionChart);
				const ProjectionSingularityBasisSample positiveSide =
					evaluateProjectionSingularityBasis(
						camera,
						projectionTangentMagnitude,
						kDirectionDotEpsilon, historyUpSign, primeProjectionChart);
				expect(negativeSide.realizedDirectionDot < 0.0f
				       && positiveSide.realizedDirectionDot > 0.0f,
				       "projection singularity fixture did not straddle directionDot=0");

				const float adjacentAngle = maxLightBasisAngleRadians(negativeSide.axes, positiveSide.axes);
				g_maxProjectionSingularityAdjacentAngleRadians = std::max(
					g_maxProjectionSingularityAdjacentAngleRadians, adjacentAngle);
				expect(glm::dot(negativeSide.axes.up, positiveSide.axes.up) > 0.99999f,
				       "directionDot=0 float32 crossing flipped light up near previousUp");
				expect(glm::dot(negativeSide.axes.right, positiveSide.axes.right) > 0.99999f,
				       "directionDot=0 float32 crossing flipped light right near previousUp");
				expect(adjacentAngle < kMaximumPairAngleRadians,
				       "directionDot=0 float32 crossing changed the persisted light basis");
			}
		}
	}

	void testAnalyticChartsHandleExactPreviousUpDirections()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		const glm::dvec3 previousDirection(0.0, -1.0, 0.0);
		constexpr float kNeighborhoodTangent = 1.0e-4f;
		constexpr float kMaximumExactAnalyticAngleRadians = 2.0e-6f;
		constexpr float kMaximumExactToNeighborBasisAngleRadians = 5.0e-4f;
		constexpr float kMaximumNextafterInputAngleRadians = 1.0e-9f;
		constexpr float kMaximumNextafterBasisAngleRadians = 2.0e-6f;

		for (const bool primeProjectionChart : std::array<bool, 2>{false, true})
		{
			for (const float historyUpSign : std::array<float, 2>{-1.0f, 1.0f})
			{
				const ProjectionSingularityBasisSample exact = evaluateProjectionSingularityBasis(
					camera, 0.0f, 0.0f, historyUpSign, primeProjectionChart);
				expect(exact.realizedProjectedLengthSq == 0.0f,
				       "exact previousUp target did not realize a zero projection tangent");

				const glm::dvec3 direction = glm::normalize(glm::dvec3(exact.realizedDirection));
				const glm::dvec3 previousUp = glm::normalize(glm::dvec3(exact.historyAxes.up));
				const glm::dvec3 chartDifference = primeProjectionChart
					? direction - previousDirection
					: previousDirection + direction;
				const double chartScale = 0.5 * glm::dot(chartDifference, chartDifference);
				glm::dvec3 expectedUp =
					previousUp * chartScale - chartDifference * glm::dot(previousUp, direction);
				expectedUp -= direction * glm::dot(expectedUp, direction);
				expect(glm::dot(expectedUp, expectedUp) > 1.0e-12,
				       "exact previousUp fixture unexpectedly reached the selected analytic pole");
				expectedUp = glm::normalize(expectedUp);
				const glm::dvec3 expectedRight = glm::normalize(glm::cross(direction, expectedUp));
				const float exactAnalyticAngle = std::max(
					unitVectorAngleRadians(exact.axes.up, glm::vec3(expectedUp)),
					unitVectorAngleRadians(exact.axes.right, glm::vec3(expectedRight)));
				g_maxZeroProjectionAnalyticAngleRadians = std::max(
					g_maxZeroProjectionAnalyticAngleRadians, exactAnalyticAngle);
				expect(exactAnalyticAngle < kMaximumExactAnalyticAngleRadians,
				       "zero commonUp projection bypassed a valid selected analytic chart");

				for (const bool useHistoryRightTangent : std::array<bool, 2>{false, true})
				{
					for (const float tangentSign : std::array<float, 2>{-1.0f, 1.0f})
					{
						const float tangent = tangentSign * kNeighborhoodTangent;
						const float adjacentTangent = std::nextafter(
							tangent,
							tangentSign < 0.0f
								? -std::numeric_limits<float>::infinity()
								: std::numeric_limits<float>::infinity());
						const ProjectionSingularityBasisSample neighbor = evaluateProjectionSingularityBasis(
							camera,
							useHistoryRightTangent ? tangent : 0.0f,
							useHistoryRightTangent ? 0.0f : tangent,
							historyUpSign,
							primeProjectionChart);
						const ProjectionSingularityBasisSample adjacent = evaluateProjectionSingularityBasis(
							camera,
							useHistoryRightTangent ? adjacentTangent : 0.0f,
							useHistoryRightTangent ? 0.0f : adjacentTangent,
							historyUpSign,
							primeProjectionChart);
						expect(neighbor.realizedProjectedLengthSq > 0.0f
						       && adjacent.realizedProjectedLengthSq > 0.0f,
						       "nextafter previousUp neighborhood collapsed to the exact zero tangent");
						expect(maxLightBasisAngleRadians(exact.axes, neighbor.axes)
						           < kMaximumExactToNeighborBasisAngleRadians,
						       "selected analytic chart was discontinuous at an exact previousUp target");

						const float nextafterInputAngle = unitVectorAngleRadians(
							neighbor.realizedDirection, adjacent.realizedDirection);
						const float nextafterBasisAngle = maxLightBasisAngleRadians(
							neighbor.axes, adjacent.axes);
						expect(nextafterInputAngle > 0.0f
						       && nextafterInputAngle < kMaximumNextafterInputAngleRadians,
						       "previousUp nextafter fixture did not preserve neighboring directions");
						expect(nextafterBasisAngle < kMaximumNextafterBasisAngleRadians,
						       "previousUp nextafter neighborhood contains a selected-chart seam");
					}
				}
			}
		}
	}

	void testPersistedProjectionChartCrossesPreviousUpContinuously()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		const float tangentMagnitude =
			std::nextafter(1.0e-4f, std::numeric_limits<float>::infinity());
		constexpr float kExpectedInputPairAngleRadians = 2.0e-4f;
		constexpr float kMaximumBasisPairAngleRadians = 5.0e-4f;

		for (const float historyUpSign : std::array<float, 2>{-1.0f, 1.0f})
		{
			const ProjectionSingularityBasisSample negativeSide =
				evaluateProjectionSingularityBasis(
					camera, -tangentMagnitude, 0.0f, historyUpSign, true);
			const ProjectionSingularityBasisSample positiveSide =
				evaluateProjectionSingularityBasis(
					camera, tangentMagnitude, 0.0f, historyUpSign, true);
			const float inputPairAngle = unitVectorAngleRadians(
				negativeSide.realizedDirection, positiveSide.realizedDirection);
			expect(nearlyEqual(inputPairAngle, kExpectedInputPairAngleRadians, 2.0e-7f),
			       "persisted projection-chart fixture did not realize the 0.0114592-degree crossing");

			const float basisPairAngle =
				maxLightBasisAngleRadians(negativeSide.axes, positiveSide.axes);
			g_maxProjectionSingularityAdjacentAngleRadians = std::max(
				g_maxProjectionSingularityAdjacentAngleRadians, basisPairAngle);
			expect(basisPairAngle < kMaximumBasisPairAngleRadians,
			       "persisted projection chart flipped across its raw previousUp singularity");
		}
	}

	void testTransportChartOneUlpTargetMapIsContinuous()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		constexpr float kTransportTangentCoefficient = -0.000244140625f;
		const float adjacentTransportTangentCoefficient = std::nextafter(
			kTransportTangentCoefficient, std::numeric_limits<float>::infinity());
		const SameHistoryTargetBasisSample lowerSide = evaluateCanonicalSameHistoryTarget(
			camera, -1.0f, kTransportTangentCoefficient, 0.0f, false);
		const SameHistoryTargetBasisSample upperSide = evaluateCanonicalSameHistoryTarget(
			camera, -1.0f, adjacentTransportTangentCoefficient, 0.0f, false);

		const float inputPairAngle =
			unitVectorAngleRadians(lowerSide.realizedDirection, upperSide.realizedDirection);
		expect(inputPairAngle > 0.0f && inputPairAngle < 5.0e-11f,
		       "transport ULP fixture did not preserve Zeno's neighboring target pair");

		constexpr float kMaximumBasisPairAngleRadians = 1.0e-7f;
		const float basisPairAngle = maxLightBasisAngleRadians(lowerSide.axes, upperSide.axes);
		g_maxProjectionSingularityAdjacentAngleRadians = std::max(
			g_maxProjectionSingularityAdjacentAngleRadians, basisPairAngle);
		expect(basisPairAngle < kMaximumBasisPairAngleRadians,
		       "one ULP in the near-antipodal transport tangent caused a target-map seam");
	}

	void testAntipodalChartNearbyTargetMapIsContinuous()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		// This canonical same-history pair produced a 117.3117-degree basis jump in
		// the normalized fade/bridge implementation for a 0.002864784-degree input edit.
		const SameHistoryTargetBasisSample first = evaluateCanonicalSameHistoryTarget(
			camera,
			-0.00134547600375f,
			-1.0f,
			-0.000531814579004f,
			true);
		const SameHistoryTargetBasisSample second = evaluateCanonicalSameHistoryTarget(
			camera,
			-0.00131901197346f,
			-1.0f,
			-0.000574236921007f,
			true);
		const float inputPairAngle =
			unitVectorAngleRadians(first.realizedDirection, second.realizedDirection);
		expect(nearlyEqual(inputPairAngle, 5.0e-5f, 1.0e-7f),
		       "antipodal-chart fixture did not realize the 0.002865-degree target edit");

		constexpr float kMaximumBasisPairAngleRadians = 2.0e-4f;
		const float basisPairAngle = maxLightBasisAngleRadians(first.axes, second.axes);
		g_maxProjectionSingularityAdjacentAngleRadians = std::max(
			g_maxProjectionSingularityAdjacentAngleRadians, basisPairAngle);
		expect(basisPairAngle < kMaximumBasisPairAngleRadians,
		       "nearby targets in the persisted antipodal chart caused a target-map seam");
	}

	void testComplementaryChartsHandleExactPoles()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		const glm::vec3 initialDirection(0.0f, -1.0f, 0.0f);
		demo::CSMShadowResources csm;
		csm.updateCascadeMatrices(camera, initialDirection, kShadowDistance);
		const LightFrameAxes initialAxes =
			extractLightFrameAxes(csm.getFrameData().cascades[0].lightView);

		csm.updateCascadeMatrices(camera, -initialDirection, kShadowDistance);
		verifyLightFrameAxes(csm.getFrameData().cascades[0].lightView);
		const LightFrameAxes antipodalAxes =
			extractLightFrameAxes(csm.getFrameData().cascades[0].lightView);
		expect(glm::dot(initialAxes.up, antipodalAxes.up) > 0.99999f,
		       "complementary chart did not preserve roll at the exact antipode");

		csm.updateCascadeMatrices(camera, initialDirection, kShadowDistance);
		verifyLightFrameAxes(csm.getFrameData().cascades[0].lightView);
		const LightFrameAxes returnedAxes =
			extractLightFrameAxes(csm.getFrameData().cascades[0].lightView);
		expect(glm::dot(initialAxes.up, returnedAxes.up) > 0.99999f,
		       "antipodal chart did not preserve roll on the exact return edit");

		csm.updateCascadeMatrices(camera, initialDirection, kShadowDistance);
		verifyLightFrameAxes(csm.getFrameData().cascades[0].lightView);
		const LightFrameAxes settledAxes =
			extractLightFrameAxes(csm.getFrameData().cascades[0].lightView);
		expect(maxLightBasisAngleRadians(returnedAxes, settledAxes) < 1.0e-6f,
		       "exact no-motion pole did not switch back to the complementary chart cleanly");
	}

	void testScaleInvariantPoleReproductions()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		const glm::vec3 historySetupDirection(
			-0.0065027643f, 0.0192404594f, 0.0162101109f);
		constexpr float kSmallScale = 0.125f;
		constexpr float kLargeScale = 7.0f;
		constexpr float kMaximumInputPairAngleRadians = 1.0e-6f;
		constexpr float kMaximumBasisPairAngleRadians = 2.0e-6f;

		for (const bool primeAntipodalChart : std::array<bool, 2>{false, true})
		{
			const bool targetAntipode = !primeAntipodalChart;
			const ScaleInvariantPoleBasisSample smallScale = evaluateScaleInvariantPoleBasis(
				camera, historySetupDirection, kSmallScale, primeAntipodalChart, targetAntipode);
			const ScaleInvariantPoleBasisSample largeScale = evaluateScaleInvariantPoleBasis(
				camera, historySetupDirection, kLargeScale, primeAntipodalChart, targetAntipode);
			expect(vectorsNearlyEqual(
				smallScale.historyDirection, largeScale.historyDirection, 0.0f),
			       "scale-invariant pole repro did not preserve an identical history direction");

			const float inputPairAngle = unitVectorAngleRadians(
				smallScale.realizedDirection, largeScale.realizedDirection);
			const float basisPairAngle =
				maxLightBasisAngleRadians(smallScale.axes, largeScale.axes);
			std::cout << "CSM scale pole repro (degrees): chart="
			          << (primeAntipodalChart ? "antipodal/no-motion" : "transport/antipode")
			          << ", input=" << glm::degrees(inputPairAngle)
			          << ", basis=" << glm::degrees(basisPairAngle) << '\n';
			expect(inputPairAngle < kMaximumInputPairAngleRadians,
			       "scale-invariant pole repro targets were not representational neighbors");
			expect(basisPairAngle < kMaximumBasisPairAngleRadians,
			       "collinear target magnitudes selected incompatible pole orientations");
		}
	}

	void testComponentUlpPoleBoundaryIsRegularized()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		const glm::vec3 historySetupDirection(
			-0.0065027643f, 0.0192404594f, 0.0162101109f);
		const glm::vec3 targetDirection(
			0.0312809013f, -0.0925535858f, -0.0779765174f);
		glm::vec3 adjacentTargetDirection = targetDirection;
		adjacentTargetDirection.y = std::nextafter(
			adjacentTargetDirection.y, std::numeric_limits<float>::infinity());

		const ScaleInvariantPoleBasisSample first = evaluateExplicitPoleTargetBasis(
			camera, historySetupDirection, false, targetDirection);
		const ScaleInvariantPoleBasisSample second = evaluateExplicitPoleTargetBasis(
			camera, historySetupDirection, false, adjacentTargetDirection);
		expect(vectorsNearlyEqual(first.historyDirection, second.historyDirection, 0.0f),
		       "component-ULP repro did not preserve an identical persisted history");

		const double firstChord = selectedPoleChordLength(first, true);
		const double secondChord = selectedPoleChordLength(second, true);
		const double lowerChord = std::min(firstChord, secondChord);
		const double upperChord = std::max(firstChord, secondChord);
		const float inputPairAngle = unitVectorAngleRadians(
			first.realizedDirection, second.realizedDirection);
		const float basisPairAngle = maxLightBasisAngleRadians(first.axes, second.axes);
		g_maxRegularizationComponentUlpAngleRadians = std::max(
			g_maxRegularizationComponentUlpAngleRadians, basisPairAngle);

		expect(lowerChord > 1.90e-6 && lowerChord < 1.93e-6
		       && upperChord > 1.92e-6 && upperChord < 1.95e-6,
		       "component-ULP fixture no longer reproduces the 16-epsilon pole boundary");
		expect(lowerChord > kLightBasisRegularizationInnerChord
		       && upperChord < kLightBasisRegularizationOuterChord,
		       "component-ULP repro did not exercise the smooth regularization annulus");
		expect(inputPairAngle > 8.0e-8f && inputPairAngle < 1.1e-7f,
		       "component-ULP fixture no longer has the reported neighboring input angle");
		expect(basisPairAngle < inputPairAngle * 3.0f + 1.0e-8f
		       && basisPairAngle < 3.0e-7f,
		       "single-component ULP near the selected pole was excessively amplified");
	}

	void testRegularizationAnnulusBoundariesAreSmooth()
	{
		static_assert(kLightBasisRegularizationOuterChord
		                  == 64.0 * kLightBasisRegularizationInnerChord,
		              "the regularization annulus ratio must stay precision-derived");
		static_assert(kLightBasisRegularizationOuterChord < 1.5e-4,
		              "the regularization annulus must stay in a float-precision neighborhood");
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		const glm::vec3 historySetupDirection(
			-0.0065027643f, 0.0192404594f, 0.0162101109f);
		const std::array<double, 2> boundaries{
			kLightBasisRegularizationInnerChord,
			kLightBasisRegularizationOuterChord,
		};
		constexpr uint32_t kRadialAzimuthSteps = 64u;
		constexpr uint32_t kBoundaryAzimuthSteps = 512u;
		constexpr float kInnerBoundaryOffsetFraction = 1.0f / 8.0f;
		constexpr float kOuterBoundaryOffsetFraction = 1.0f / 64.0f;
		constexpr float kMaximumNextafterRadialInputAngleRadians = 5.0e-7f;
		constexpr float kMaximumRadialInputAngleRadians = 5.0e-6f;
		constexpr float kMaximumNextafterRadialBasisAngleRadians = 5.0e-4f;
		constexpr float kMaximumRadialBasisAngleRadians = 2.0e-3f;
		constexpr float kMaximumAzimuthBasisAngleRadians = 4.0e-2f;

		for (const bool primeAntipodalChart : std::array<bool, 2>{false, true})
		{
			const bool targetAntipode = !primeAntipodalChart;
			for (const double boundary : boundaries)
			{
				const float boundaryRadius = static_cast<float>(boundary);
				const float boundaryOffsetFraction = boundary == boundaries.front()
					? kInnerBoundaryOffsetFraction
					: kOuterBoundaryOffsetFraction;
				const std::array<std::array<float, 2>, 2> radialPairs{
					std::array<float, 2>{
						std::nextafter(boundaryRadius, 0.0f),
						std::nextafter(boundaryRadius, std::numeric_limits<float>::infinity())},
					std::array<float, 2>{
						boundaryRadius * (1.0f - boundaryOffsetFraction),
						boundaryRadius * (1.0f + boundaryOffsetFraction)},
				};
				for (size_t radialPairIndex = 0; radialPairIndex < radialPairs.size(); ++radialPairIndex)
				{
					const auto& radialPair = radialPairs[radialPairIndex];
					for (uint32_t azimuthIndex = 0; azimuthIndex < kRadialAzimuthSteps; ++azimuthIndex)
					{
						const float azimuth = glm::two_pi<float>()
							* static_cast<float>(azimuthIndex)
							/ static_cast<float>(kRadialAzimuthSteps);
						const ScaleInvariantPoleBasisSample lower = evaluateScaleInvariantPoleBasis(
							camera, historySetupDirection, 1.0f, primeAntipodalChart, targetAntipode,
							radialPair[0] * std::cos(azimuth), radialPair[0] * std::sin(azimuth));
						const ScaleInvariantPoleBasisSample upper = evaluateScaleInvariantPoleBasis(
							camera, historySetupDirection, 1.0f, primeAntipodalChart, targetAntipode,
							radialPair[1] * std::cos(azimuth), radialPair[1] * std::sin(azimuth));
						const float inputPairAngle = unitVectorAngleRadians(
							lower.realizedDirection, upper.realizedDirection);
						const float basisPairAngle = maxLightBasisAngleRadians(lower.axes, upper.axes);
						const double lowerChord = selectedPoleChordLength(lower, targetAntipode);
						const double upperChord = selectedPoleChordLength(upper, targetAntipode);
						if (radialPairIndex == 1u)
						{
							expect(std::min(lowerChord, upperChord) < boundary
							       && std::max(lowerChord, upperChord) > boundary,
							       "expanded radial pair did not straddle the realized annulus boundary");
						}
						g_maxRegularizationBoundaryRadialAngleRadians = std::max(
							g_maxRegularizationBoundaryRadialAngleRadians, basisPairAngle);
						const float maximumInputAngle = radialPairIndex == 0u
							? kMaximumNextafterRadialInputAngleRadians
							: kMaximumRadialInputAngleRadians;
						expect(inputPairAngle < maximumInputAngle,
						       "regularization radial boundary fixture was not locally adjacent");
						const float maximumBasisAngle = radialPairIndex == 0u
							? kMaximumNextafterRadialBasisAngleRadians
							: kMaximumRadialBasisAngleRadians;
						expect(basisPairAngle < maximumBasisAngle,
						       "regularization introduced a radial seam at an annulus boundary");
					}
				}

				for (const float radius : std::array<float, 2>{
					boundaryRadius * (1.0f - boundaryOffsetFraction),
					boundaryRadius * (1.0f + boundaryOffsetFraction)})
				{
					ScaleInvariantPoleBasisSample first{};
					ScaleInvariantPoleBasisSample previous{};
					float maximumCircleBasisAngle = 0.0f;
					for (uint32_t azimuthIndex = 0; azimuthIndex < kBoundaryAzimuthSteps; ++azimuthIndex)
					{
						const float azimuth = glm::two_pi<float>()
							* static_cast<float>(azimuthIndex)
							/ static_cast<float>(kBoundaryAzimuthSteps);
						const ScaleInvariantPoleBasisSample sample = evaluateScaleInvariantPoleBasis(
							camera, historySetupDirection, 1.0f, primeAntipodalChart, targetAntipode,
							radius * std::cos(azimuth), radius * std::sin(azimuth));
						if (azimuthIndex == 0u)
						{
							first = sample;
						}
						else
						{
							maximumCircleBasisAngle = std::max(maximumCircleBasisAngle,
								maxLightBasisAngleRadians(previous.axes, sample.axes));
						}
						previous = sample;
					}
					maximumCircleBasisAngle = std::max(maximumCircleBasisAngle,
						maxLightBasisAngleRadians(previous.axes, first.axes));
					g_maxRegularizationBoundaryAzimuthAngleRadians = std::max(
						g_maxRegularizationBoundaryAzimuthAngleRadians, maximumCircleBasisAngle);
					expect(maximumCircleBasisAngle < kMaximumAzimuthBasisAngleRadians,
					       "regularization signed-roll cut leaked onto an annulus boundary");
				}
			}
		}

		// The selected analytic chart has non-zero roll winding around its pole,
		// while the common projected-up reference has zero winding. A non-vanishing
		// tangent basis cannot continuously extend both over the full annulus. The
		// hemisphere-aligned construction confines that unavoidable gauge cut to the
		// open, float-precision-sized annulus; these scans enforce smooth inner and
		// outer boundaries instead of asserting impossible path-independent continuity.
	}
	void testScaleInvariantPoleScaleSweep()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		const glm::vec3 historySetupDirection(
			-0.0065027643f, 0.0192404594f, 0.0162101109f);
		const std::array<float, 11> targetScales{
			std::ldexp(1.0f, -80),
			std::ldexp(1.0f, -40),
			0.125f,
			0.3f,
			std::nextafter(1.0f, 0.0f),
			1.0f,
			std::nextafter(1.0f, std::numeric_limits<float>::infinity()),
			7.0f,
			12345.67f,
			std::ldexp(1.0f, 40),
			std::ldexp(1.0f, 80),
		};
		constexpr float kMaximumInputAngleRadians =
			static_cast<float>(kLightBasisRegularizationInnerChord);
		constexpr float kMaximumBasisAngleRadians = 4.0e-6f;

		for (const bool primeAntipodalChart : std::array<bool, 2>{false, true})
		{
			const bool targetAntipode = !primeAntipodalChart;
			const ScaleInvariantPoleBasisSample reference = evaluateScaleInvariantPoleBasis(
				camera, historySetupDirection, 1.0f, primeAntipodalChart, targetAntipode);
			for (const float targetScale : targetScales)
			{
				const ScaleInvariantPoleBasisSample sample = evaluateScaleInvariantPoleBasis(
					camera, historySetupDirection, targetScale, primeAntipodalChart, targetAntipode);
				expect(vectorsNearlyEqual(reference.historyDirection, sample.historyDirection, 0.0f),
				       "scale sweep changed the persisted pole-test history");
				expect(unitVectorAngleRadians(
					reference.realizedDirection, sample.realizedDirection) < kMaximumInputAngleRadians,
				       "finite normal-range scaling exceeded the regularized pole neighborhood");
				expect(maxLightBasisAngleRadians(reference.axes, sample.axes)
				           < kMaximumBasisAngleRadians,
				       "finite normal-range scaling changed the canonical pole orientation");
			}
		}
	}

	void testRegularizationRandomNonAxisHistories()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		uint32_t randomState = 0xA341316Cu;
		auto nextRandomUnit = [&randomState]() mutable
		{
			randomState = randomState * 1664525u + 1013904223u;
			return static_cast<float>(randomState >> 8u) * (1.0f / 16777216.0f);
		};
		constexpr uint32_t kHistoryCount = 24u;
		constexpr float kMaximumScaleBasisAngleRadians = 5.0e-6f;
		constexpr float kMaximumUlpBasisAngleRadians = 5.0e-6f;

		for (uint32_t historyIndex = 0; historyIndex < kHistoryCount; ++historyIndex)
		{
			glm::vec3 historySetupDirection(0.0f);
			for (;;)
			{
				historySetupDirection = glm::vec3(
					nextRandomUnit() * 2.0f - 1.0f,
					nextRandomUnit() * 2.0f - 1.0f,
					nextRandomUnit() * 2.0f - 1.0f);
				if (glm::dot(historySetupDirection, historySetupDirection) <= 0.2f)
				{
					continue;
				}
				const glm::vec3 normalizedHistory = glm::normalize(historySetupDirection);
				const glm::vec3 absoluteHistory = glm::abs(normalizedHistory);
				if (std::min({absoluteHistory.x, absoluteHistory.y, absoluteHistory.z}) > 0.04f
				    && std::max({absoluteHistory.x, absoluteHistory.y, absoluteHistory.z}) < 0.96f)
				{
					break;
				}
			}

			for (const bool primeAntipodalChart : std::array<bool, 2>{false, true})
			{
				const bool targetAntipode = !primeAntipodalChart;
				const ScaleInvariantPoleBasisSample smallScale = evaluateScaleInvariantPoleBasis(
					camera, historySetupDirection, 0.125f, primeAntipodalChart, targetAntipode);
				const ScaleInvariantPoleBasisSample largeScale = evaluateScaleInvariantPoleBasis(
					camera, historySetupDirection, 7.0f, primeAntipodalChart, targetAntipode);
				expect(unitVectorAngleRadians(
					smallScale.realizedDirection, largeScale.realizedDirection)
				       < static_cast<float>(kLightBasisRegularizationInnerChord),
				       "random non-axis scale pair exceeded the numerical pole neighborhood");
				expect(maxLightBasisAngleRadians(smallScale.axes, largeScale.axes)
				           < kMaximumScaleBasisAngleRadians,
				       "random non-axis collinear scaling changed the pole orientation");

				const float azimuth = glm::two_pi<float>() * nextRandomUnit();
				const float radius = static_cast<float>(kLightBasisRegularizationInnerChord)
					* (0.75f + 0.30f * nextRandomUnit());
				const float targetScale = ((historyIndex + (primeAntipodalChart ? 1u : 0u)) & 1u)
					? 7.0f : 0.125f;
				const glm::vec3 poleDirection = targetAntipode
					? -smallScale.historyDirection : smallScale.historyDirection;
				const glm::vec3 targetDirection = (
					poleDirection
					+ smallScale.historyAxes.up * (radius * std::cos(azimuth))
					+ smallScale.historyAxes.right * (radius * std::sin(azimuth))) * targetScale;
				glm::vec3 adjacentTargetDirection = targetDirection;
				const uint32_t component = (historyIndex + (primeAntipodalChart ? 1u : 0u)) % 3u;
				const float toward = ((historyIndex + component) & 1u)
					? std::numeric_limits<float>::infinity()
					: -std::numeric_limits<float>::infinity();
				adjacentTargetDirection[component] = std::nextafter(
					adjacentTargetDirection[component], toward);

				const ScaleInvariantPoleBasisSample first = evaluateExplicitPoleTargetBasis(
					camera, historySetupDirection, primeAntipodalChart, targetDirection);
				const ScaleInvariantPoleBasisSample second = evaluateExplicitPoleTargetBasis(
					camera, historySetupDirection, primeAntipodalChart, adjacentTargetDirection);
				const float inputPairAngle = unitVectorAngleRadians(
					first.realizedDirection, second.realizedDirection);
				const float basisPairAngle = maxLightBasisAngleRadians(first.axes, second.axes);
				g_maxRegularizationRandomUlpAngleRadians = std::max(
					g_maxRegularizationRandomUlpAngleRadians, basisPairAngle);
				expect(std::max(
					selectedPoleChordLength(first, targetAntipode),
					selectedPoleChordLength(second, targetAntipode))
				       < kLightBasisRegularizationInnerChord * 1.5,
				       "random component-ULP fixture escaped the regularized pole neighborhood");
				expect(basisPairAngle < inputPairAngle * 512.0f + 1.0e-6f
				       && basisPairAngle < kMaximumUlpBasisAngleRadians,
				       "random non-axis component ULP was unbounded near a selected chart pole");
			}
		}
	}
	void testLightBasisRandomSmallStepSpherePath()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		demo::CSMShadowResources csm;
		csm.updateCascadeMatrices(
			camera, glm::normalize(glm::vec3(0.31f, -0.72f, -0.62f)), kShadowDistance);
		glm::vec3 currentDirection = csm.getFrameData().lightDirection;
		LightFrameAxes previousAxes =
			extractLightFrameAxes(csm.getFrameData().cascades[0].lightView);
		verifyLightFrameAxes(csm.getFrameData().cascades[0].lightView);

		uint32_t randomState = 0x51A7E2D3u;
		auto nextRandomUnit = [&randomState]() mutable
		{
			randomState = randomState * 1664525u + 1013904223u;
			return static_cast<float>(randomState >> 8u) * (1.0f / 16777216.0f);
		};
		constexpr uint32_t kRandomStepCount = 2048u;
		constexpr float kMaximumDirectionStepRadians = 0.012f;
		constexpr float kMaximumBasisStepRadians = 0.03f;
		for (uint32_t stepIndex = 0; stepIndex < kRandomStepCount; ++stepIndex)
		{
			glm::vec3 randomVector(
				nextRandomUnit() * 2.0f - 1.0f,
				nextRandomUnit() * 2.0f - 1.0f,
				nextRandomUnit() * 2.0f - 1.0f);
			glm::vec3 tangent =
				randomVector - currentDirection * glm::dot(randomVector, currentDirection);
			if (glm::dot(tangent, tangent) < 1.0e-6f)
			{
				tangent = previousAxes.right;
			}
			tangent = glm::normalize(tangent);
			const float directionStepRadians =
				(0.25f + 0.75f * nextRandomUnit()) * kMaximumDirectionStepRadians;
			const glm::vec3 nextDirection = glm::normalize(
				currentDirection * std::cos(directionStepRadians)
				+ tangent * std::sin(directionStepRadians));
			csm.updateCascadeMatrices(camera, nextDirection, kShadowDistance);

			const auto& frame = csm.getFrameData();
			verifyLightFrameAxes(frame.cascades[0].lightView);
			const LightFrameAxes axes = extractLightFrameAxes(frame.cascades[0].lightView);
			const float adjacentAngle = maxLightBasisAngleRadians(previousAxes, axes);
			g_maxRandomSpherePathAdjacentAngleRadians = std::max(
				g_maxRandomSpherePathAdjacentAngleRadians, adjacentAngle);
			expect(adjacentAngle < kMaximumBasisStepRadians,
			       "random small-step sphere path contains a light-basis jump");
			currentDirection = frame.lightDirection;
			previousAxes = axes;
		}
	}

	void testLightBasisClosedGreatCircleScan()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		const glm::vec3 loopNormal = glm::normalize(glm::vec3(0.37f, 0.81f, -0.45f));
		const glm::vec3 loopAxisX = glm::normalize(
			glm::cross(loopNormal, glm::normalize(glm::vec3(-0.28f, 0.51f, 0.81f))));
		const glm::vec3 loopAxisY = glm::normalize(glm::cross(loopNormal, loopAxisX));
		constexpr uint32_t kLoopStepCount = 1440u;
		constexpr float kTwoPi = 6.28318530717958647692f;
		constexpr float kMaximumBasisStepRadians = 0.02f;

		demo::CSMShadowResources csm;
		csm.updateCascadeMatrices(camera, loopAxisX, kShadowDistance);
		const glm::vec3 initialDirection = csm.getFrameData().lightDirection;
		LightFrameAxes previousAxes =
			extractLightFrameAxes(csm.getFrameData().cascades[0].lightView);
		verifyLightFrameAxes(csm.getFrameData().cascades[0].lightView);
		for (uint32_t stepIndex = 1; stepIndex <= kLoopStepCount; ++stepIndex)
		{
			const float phase =
				kTwoPi * static_cast<float>(stepIndex) / static_cast<float>(kLoopStepCount);
			const glm::vec3 targetDirection = glm::normalize(
				loopAxisX * std::cos(phase) + loopAxisY * std::sin(phase));
			csm.updateCascadeMatrices(camera, targetDirection, kShadowDistance);

			const auto& frame = csm.getFrameData();
			verifyLightFrameAxes(frame.cascades[0].lightView);
			const LightFrameAxes axes = extractLightFrameAxes(frame.cascades[0].lightView);
			const float adjacentAngle = maxLightBasisAngleRadians(previousAxes, axes);
			g_maxClosedLoopAdjacentAngleRadians = std::max(
				g_maxClosedLoopAdjacentAngleRadians, adjacentAngle);
			expect(adjacentAngle < kMaximumBasisStepRadians,
			       "closed great-circle scan contains a light-basis jump");
			previousAxes = axes;
		}
		expect(glm::dot(initialDirection, csm.getFrameData().lightDirection) > 0.999999f,
		       "closed great-circle scan did not return to its starting direction");
	}

	void testOpposedChartTeleportPerturbationDoesNotFlipRoll()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		const float requestedDirectionDot = -1.0f + std::sqrt(1.0e-3f);
		constexpr float kChartPlaneNormalEpsilon = 1.0e-7f;
		constexpr float kMaximumPairAngleRadians = 1.0e-3f;

		for (const bool primeProjectionChart : std::array<bool, 2>{false, true})
		{
			const ChartOverlapTeleportSample negativeSide = evaluateChartOverlapTeleport(
				camera, requestedDirectionDot, -kChartPlaneNormalEpsilon, primeProjectionChart);
			const ChartOverlapTeleportSample positiveSide = evaluateChartOverlapTeleport(
				camera, requestedDirectionDot, kChartPlaneNormalEpsilon, primeProjectionChart);
			expect(nearlyEqual(negativeSide.realizedDirectionDot, requestedDirectionDot, 1.0e-6f)
			       && nearlyEqual(positiveSide.realizedDirectionDot, requestedDirectionDot, 1.0e-6f),
			       "chart-overlap teleport fixture did not realize directionDot=-1+sqrt(1e-3)");
			expect(nearlyEqual(negativeSide.realizedTransportedLengthSq, 1.0e-3f, 2.0e-7f)
			       && nearlyEqual(positiveSide.realizedTransportedLengthSq, 1.0e-3f, 2.0e-7f),
			       "chart-overlap teleport fixture did not realize transportedLengthSq=1e-3");
			expect(negativeSide.realizedPlaneNormal < 0.0f
			       && positiveSide.realizedPlaneNormal > 0.0f,
			       "chart-overlap teleport fixture did not straddle its plane normal");
			expect(negativeSide.realizedChartAlignment < -0.9999f
			       && positiveSide.realizedChartAlignment < -0.9999f,
			       "chart-overlap teleport fixture did not create opposed charts");
			expect(glm::dot(negativeSide.realizedDirection, positiveSide.realizedDirection) > 0.999999f,
			       "chart-overlap perturbation changed the teleport direction materially");
			expect(glm::dot(negativeSide.historyAxes.back, negativeSide.axes.back) < -0.96f
			       && glm::dot(positiveSide.historyAxes.back, positiveSide.axes.back) < -0.96f,
			       "chart-overlap fixture did not preserve the real large light-direction redirect");

			const float pairAngle = maxLightBasisAngleRadians(negativeSide.axes, positiveSide.axes);
			g_maxChartOverlapTeleportPairAngleRadians = std::max(
				g_maxChartOverlapTeleportPairAngleRadians, pairAngle);
			expect(pairAngle < kMaximumPairAngleRadians,
			       "tiny perturbation of one large light teleport added a 180-degree basis roll");
		}
	}

	void testOpposedChartLargeTeleportNeighborhoodScan()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(4.0f, 7.0f, 11.0f), glm::normalize(glm::vec3(-0.25f, -0.18f, -1.0f)));
		const float centerDirectionDot = -1.0f + std::sqrt(1.0e-3f);
		constexpr std::array<float, 5> kDirectionDotOffsets{
			-4.0e-5f,
			-2.0e-5f,
			0.0f,
			2.0e-5f,
			4.0e-5f,
		};
		constexpr uint32_t kPlaneSampleCount = 129u;
		constexpr float kPlaneNormalExtent = 2.0e-4f;
		constexpr float kMaximumAdjacentAngleRadians = 5.0e-3f;

		for (const bool primeProjectionChart : std::array<bool, 2>{false, true})
		{
			std::array<LightFrameAxes, kPlaneSampleCount> previousRowAxes{};
			bool havePreviousRow = false;
			for (const float directionDotOffset : kDirectionDotOffsets)
			{
				LightFrameAxes previousAxesInRow{};
				bool havePreviousInRow = false;
				for (uint32_t sampleIndex = 0; sampleIndex < kPlaneSampleCount; ++sampleIndex)
				{
					const float interpolation =
						static_cast<float>(sampleIndex) / static_cast<float>(kPlaneSampleCount - 1u);
					const float planeNormal = glm::mix(
						-kPlaneNormalExtent, kPlaneNormalExtent, interpolation);
					const ChartOverlapTeleportSample sample = evaluateChartOverlapTeleport(
						camera,
						centerDirectionDot + directionDotOffset,
						planeNormal,
						primeProjectionChart);
					expect(sample.realizedChartAlignment < -0.99f,
					       "large-teleport neighborhood left the opposed chart overlap");

					if (havePreviousInRow)
					{
						const float adjacentAngle =
							maxLightBasisAngleRadians(previousAxesInRow, sample.axes);
						g_maxLargeTeleportNeighborhoodAdjacentAngleRadians = std::max(
							g_maxLargeTeleportNeighborhoodAdjacentAngleRadians, adjacentAngle);
						expect(adjacentAngle < kMaximumAdjacentAngleRadians,
						       "large-teleport plane-normal scan contains a basis roll seam");
					}
					if (havePreviousRow)
					{
						const float adjacentAngle =
							maxLightBasisAngleRadians(previousRowAxes[sampleIndex], sample.axes);
						g_maxLargeTeleportNeighborhoodAdjacentAngleRadians = std::max(
							g_maxLargeTeleportNeighborhoodAdjacentAngleRadians, adjacentAngle);
						expect(adjacentAngle < kMaximumAdjacentAngleRadians,
						       "large-teleport direction-dot scan contains a basis roll seam");
					}

					previousAxesInRow = sample.axes;
					previousRowAxes[sampleIndex] = sample.axes;
					havePreviousInRow = true;
				}
				havePreviousRow = true;
			}
		}
	}

	void testRuntimeCasterShadowPackedState()
	{
		demo::ShadowPackedMesh packedMesh{};
		packedMesh.meshIndex = 3u;
		packedMesh.drawRecordIndex = 17u;
		packedMesh.drawData.baseColorFactor = glm::vec4(0.2f, 0.4f, 0.6f, 0.8f);
		const glm::mat4 uploadTransform = glm::translate(glm::mat4(1.0f), glm::vec3(-2.0f, 1.0f, 4.0f));
		packedMesh.drawData.modelMatrix = uploadTransform;
		packedMesh.drawData.prevModelMatrix = uploadTransform;
		packedMesh.boundsSphere = glm::vec4(-2.0f, 1.0f, 4.0f, 1.0f);

		const glm::mat4 parentTransform =
			glm::translate(glm::mat4(1.0f), glm::vec3(8.0f, -3.0f, 5.0f))
			* glm::rotate(glm::mat4(1.0f), glm::radians(37.0f), glm::normalize(glm::vec3(0.3f, 1.0f, -0.2f)))
			* glm::scale(glm::mat4(1.0f), glm::vec3(2.5f, 0.75f, 1.8f));
		const glm::mat4 localTransform =
			glm::translate(glm::mat4(1.0f), glm::vec3(1.5f, 2.0f, -0.5f));
		const glm::mat4 runtimeTransform = parentTransform * localTransform;
		const glm::vec4 runtimeBounds(glm::vec3(runtimeTransform * glm::vec4(0.25f, -0.5f, 0.75f, 1.0f)), 6.25f);

		demo::updateShadowPackedMeshRuntimeState(packedMesh, runtimeTransform, runtimeBounds);
		expect(matricesNearlyEqual(packedMesh.drawData.modelMatrix, runtimeTransform, 1.0e-6f),
		       "runtime caster model matrix did not reach the shadow-packed draw source");
		expect(matricesNearlyEqual(packedMesh.drawData.prevModelMatrix, uploadTransform, 1.0e-6f),
		       "runtime caster update discarded the previous shadow model matrix");
		expect(vectorsNearlyEqual(packedMesh.boundsSphere, runtimeBounds, 1.0e-6f),
		       "runtime caster bounds did not reach the shadow-packed culling source");
		expect(packedMesh.drawRecordIndex == 17u && packedMesh.meshIndex == 3u,
		       "runtime caster update changed shadow-packed source identity");
		expect(vectorsNearlyEqual(packedMesh.drawData.baseColorFactor, glm::vec4(0.2f, 0.4f, 0.6f, 0.8f), 1.0e-6f),
		       "runtime caster update overwrote shadow material data");

		const glm::mat4 secondTransform =
			glm::translate(glm::mat4(1.0f), glm::vec3(-4.0f, 6.0f, 2.0f)) * runtimeTransform;
		demo::updateShadowPackedMeshRuntimeState(
			packedMesh, secondTransform, glm::vec4(glm::vec3(runtimeBounds) + glm::vec3(-4.0f, 6.0f, 2.0f), 6.25f));
		expect(matricesNearlyEqual(packedMesh.drawData.prevModelMatrix, runtimeTransform, 1.0e-6f),
		       "successive runtime caster updates did not advance the shadow previous matrix");
	}

	void testSharedMeshInstanceTransformHistoryAcrossFrameSlots()
	{
		const demo::MeshHandle sharedMesh{.index = 7u, .generation = 3u};
		const std::array<demo::MeshHandle, 2> instanceMeshes{sharedMesh, sharedMesh};
		const glm::mat4 instanceZeroInitial =
			glm::translate(glm::mat4(1.0f), glm::vec3(-3.0f, 1.0f, 2.0f));
		const glm::mat4 instanceOneInitial =
			glm::translate(glm::mat4(1.0f), glm::vec3(6.0f, -2.0f, -4.0f));
		std::array<demo::SubmittedTransformHistory, 2> histories{
			demo::makeSubmittedTransformHistory(instanceZeroInitial),
			demo::makeSubmittedTransformHistory(instanceOneInitial),
		};
		std::array<glm::mat4, 2> registryWorldTransforms{instanceZeroInitial, instanceOneInitial};
		std::array<demo::ShadowPackedMesh, 2> shadowPacked{};
		for (uint32_t instanceIndex = 0; instanceIndex < shadowPacked.size(); ++instanceIndex)
		{
			shadowPacked[instanceIndex].meshIndex = 0u;
			shadowPacked[instanceIndex].drawRecordIndex = instanceIndex;
			shadowPacked[instanceIndex].drawData.modelMatrix = histories[instanceIndex].currentWorldTransform;
			shadowPacked[instanceIndex].drawData.prevModelMatrix = histories[instanceIndex].previousWorldTransform;
		}

		// Two meshlets share geometry but retain the owning scene draw record.
		const std::array<uint32_t, 4> drawRecordByMeshlet{0u, 0u, 1u, 1u};
		std::array<std::array<shaderio::DrawUniforms, 4>, 3> frameSlotPayloads{};
		const auto publishFrameSlot = [&](uint32_t frameSlot)
		{
			for (uint32_t drawIndex = 0; drawIndex < drawRecordByMeshlet.size(); ++drawIndex)
			{
				const demo::SubmittedTransformHistory& history = histories[drawRecordByMeshlet[drawIndex]];
				frameSlotPayloads[frameSlot][drawIndex].modelMatrix = history.currentWorldTransform;
				frameSlotPayloads[frameSlot][drawIndex].prevModelMatrix = history.previousWorldTransform;
			}
		};
		const auto expectInstancePayload = [&](uint32_t frameSlot,
		                                       uint32_t drawRecordIndex,
		                                       const glm::mat4& expectedPrevious,
		                                       const glm::mat4& expectedCurrent)
		{
			for (uint32_t drawIndex = 0; drawIndex < drawRecordByMeshlet.size(); ++drawIndex)
			{
				if (drawRecordByMeshlet[drawIndex] != drawRecordIndex)
				{
					continue;
				}
				expect(matricesNearlyEqual(
					frameSlotPayloads[frameSlot][drawIndex].prevModelMatrix, expectedPrevious, 1.0e-6f),
				       "meshlet draw lost the instance previous transform");
				expect(matricesNearlyEqual(
					frameSlotPayloads[frameSlot][drawIndex].modelMatrix, expectedCurrent, 1.0e-6f),
				       "meshlet draw lost the instance current transform");
			}
		};

		const glm::mat4 parentTransform =
			glm::translate(glm::mat4(1.0f), glm::vec3(9.0f, -4.0f, 5.0f))
			* glm::rotate(glm::mat4(1.0f), glm::radians(41.0f), glm::normalize(glm::vec3(0.2f, 1.0f, -0.35f)))
			* glm::scale(glm::mat4(1.0f), glm::vec3(2.75f, 0.65f, 1.4f));
		const glm::mat4 firstLocalUpdate =
			glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, -0.5f))
			* glm::rotate(glm::mat4(1.0f), glm::radians(-17.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		const glm::mat4 secondLocalUpdate =
			glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 1.5f, -1.25f))
			* glm::rotate(glm::mat4(1.0f), glm::radians(23.0f), glm::vec3(1.0f, 0.0f, 0.0f));
		const glm::mat4 frameZeroIntermediate = parentTransform * firstLocalUpdate;
		const glm::mat4 frameZeroCurrent = parentTransform * secondLocalUpdate;

		// Multiple edits before any submitted frame retain the first pre-edit matrix.
		demo::updateSubmittedTransformHistory(histories[0], frameZeroIntermediate);
		demo::updateSubmittedTransformHistory(histories[0], frameZeroCurrent);
		registryWorldTransforms[0] = histories[0].currentWorldTransform;
		const glm::vec4 frameZeroBounds(
			glm::vec3(frameZeroCurrent * glm::vec4(0.25f, -0.5f, 0.75f, 1.0f)), 5.5f);
		demo::updateShadowPackedMeshRuntimeState(
			shadowPacked[0],
			histories[0].currentWorldTransform,
			histories[0].previousWorldTransform,
			frameZeroBounds);

		// A failed/no-op submission does not publish a frame slot or advance history.
		expect(histories[0].updatedSinceLastSubmit,
		       "pending instance transform history was cleared by a no-op submission");
		expect(matricesNearlyEqual(histories[0].previousWorldTransform, instanceZeroInitial, 1.0e-6f),
		       "first-update previous transform advanced before a successful submission");
		expect(matricesNearlyEqual(registryWorldTransforms[1], instanceOneInitial, 1.0e-6f),
		       "updating one shared-mesh instance changed the other registry object");
		expect(instanceMeshes[0] == instanceMeshes[1], "shared-mesh test no longer shares geometry identity");

		publishFrameSlot(0u);
		expectInstancePayload(0u, 0u, instanceZeroInitial, frameZeroCurrent);
		expectInstancePayload(0u, 1u, instanceOneInitial, instanceOneInitial);
		expect(matricesNearlyEqual(shadowPacked[0].drawData.prevModelMatrix, instanceZeroInitial, 1.0e-6f)
		       && matricesNearlyEqual(shadowPacked[0].drawData.modelMatrix, frameZeroCurrent, 1.0e-6f),
		       "frame zero shadow-packed payload diverged from meshlet raster payload");
		demo::commitSubmittedTransformHistory(histories[0]);
		demo::commitSubmittedTransformHistory(histories[1]);
		demo::commitShadowPackedMeshSubmittedState(shadowPacked[0]);
		demo::commitShadowPackedMeshSubmittedState(shadowPacked[1]);

		publishFrameSlot(1u);
		expectInstancePayload(1u, 0u, frameZeroCurrent, frameZeroCurrent);
		expectInstancePayload(1u, 1u, instanceOneInitial, instanceOneInitial);
		expect(matricesNearlyEqual(shadowPacked[0].drawData.prevModelMatrix, frameZeroCurrent, 1.0e-6f),
		       "settled frame did not collapse shadow previous to current");

		const glm::mat4 frameTwoCurrent =
			glm::translate(glm::mat4(1.0f), glm::vec3(-2.0f, 3.0f, 1.0f)) * frameZeroCurrent;
		demo::updateSubmittedTransformHistory(histories[0], frameTwoCurrent);
		registryWorldTransforms[0] = histories[0].currentWorldTransform;
		demo::updateShadowPackedMeshRuntimeState(
			shadowPacked[0],
			histories[0].currentWorldTransform,
			histories[0].previousWorldTransform,
			glm::vec4(glm::vec3(frameTwoCurrent * glm::vec4(0.25f, -0.5f, 0.75f, 1.0f)), 5.5f));
		publishFrameSlot(2u);
		expectInstancePayload(2u, 0u, frameZeroCurrent, frameTwoCurrent);
		expectInstancePayload(2u, 1u, instanceOneInitial, instanceOneInitial);
		expect(matricesNearlyEqual(registryWorldTransforms[0], frameTwoCurrent, 1.0e-6f)
		       && matricesNearlyEqual(registryWorldTransforms[1], instanceOneInitial, 1.0e-6f),
		       "third frame registry transforms lost per-instance identity");
		expect(matricesNearlyEqual(shadowPacked[0].drawData.prevModelMatrix, frameZeroCurrent, 1.0e-6f)
		       && matricesNearlyEqual(shadowPacked[0].drawData.modelMatrix, frameTwoCurrent, 1.0e-6f),
		       "third frame shadow-packed transform diverged from drawData history");
	}
	void testMeshletDenseRemapAcrossTwoAndThreeFrameSlots()
	{
		struct DrawIdentityModel
		{
			demo::GPUSceneObjectHandle registryObject{};
			uint32_t drawRecordIndex{UINT32_MAX};
			uint32_t sceneObjectIndex{UINT32_MAX};
			bool live{true};
		};

		const demo::GPUSceneObjectHandle firstObject{.index = 1u, .generation = 1u};
		const demo::GPUSceneObjectHandle removedObject{.index = 2u, .generation = 1u};
		const demo::GPUSceneObjectHandle movedObject{.index = 3u, .generation = 1u};
		std::array<DrawIdentityModel, 6> identities{{
			{firstObject, 0u, 0u, true},
			{firstObject, 0u, 0u, true},
			{removedObject, 1u, 1u, true},
			{removedObject, 1u, 1u, true},
			{movedObject, 2u, 2u, true},
			{movedObject, 2u, 2u, true},
		}};
		std::array<uint32_t, 6> meshletObjectIndices{0u, 0u, 1u, 1u, 2u, 2u};
		std::array<uint32_t, 6> meshletIndexCounts{12u, 15u, 18u, 21u, 24u, 27u};
		const demo::GPUSceneRemoveResult removal{
			.removed = true,
			.removedObject = removedObject,
			.removedDenseIndex = 1u,
			.movedObject = movedObject,
			.movedFromDenseIndex = 2u,
			.movedToDenseIndex = 1u,
		};
		expect(removal.hasDenseRemap(), "dense-remap model did not represent a middle removal");
		for (uint32_t drawIndex = 0; drawIndex < identities.size(); ++drawIndex)
		{
			DrawIdentityModel& identity = identities[drawIndex];
			if (identity.registryObject == movedObject)
			{
				identity.sceneObjectIndex = removal.movedToDenseIndex;
				meshletObjectIndices[drawIndex] = removal.movedToDenseIndex;
			}
			else if (identity.registryObject == removedObject)
			{
				identity = {};
				identity.live = false;
				meshletObjectIndices[drawIndex] = UINT32_MAX;
				meshletIndexCounts[drawIndex] = 0u;
			}
		}

		const glm::mat4 firstInitial =
			glm::translate(glm::mat4(1.0f), glm::vec3(-5.0f, 1.0f, 2.0f));
		const glm::mat4 removedInitial =
			glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, -1.0f, 4.0f));
		const glm::mat4 movedInitial =
			glm::translate(glm::mat4(1.0f), glm::vec3(7.0f, 3.0f, -6.0f));
		const glm::mat4 parentTransform =
			glm::translate(glm::mat4(1.0f), glm::vec3(11.0f, -4.0f, 5.0f))
			* glm::rotate(glm::mat4(1.0f), glm::radians(43.0f), glm::normalize(glm::vec3(0.3f, 1.0f, -0.25f)))
			* glm::scale(glm::mat4(1.0f), glm::vec3(2.8f, 0.6f, 1.45f));
		const glm::mat4 movedFrameZero =
			parentTransform
			* glm::translate(glm::mat4(1.0f), glm::vec3(1.5f, 2.0f, -0.75f));
		const glm::mat4 movedFrameTwo =
			glm::translate(glm::mat4(1.0f), glm::vec3(-3.0f, 1.0f, 2.0f)) * movedFrameZero;

		for (const uint32_t frameSlotCount : std::array<uint32_t, 2>{2u, 3u})
		{
			std::array<demo::SubmittedTransformHistory, 3> histories{
				demo::makeSubmittedTransformHistory(firstInitial),
				demo::makeSubmittedTransformHistory(removedInitial),
				demo::makeSubmittedTransformHistory(movedInitial),
			};
			std::vector<demo::ShadowPackedMesh> shadowPacked(3);
			for (uint32_t drawRecordIndex = 0; drawRecordIndex < shadowPacked.size(); ++drawRecordIndex)
			{
				shadowPacked[drawRecordIndex].drawRecordIndex = drawRecordIndex;
				shadowPacked[drawRecordIndex].meshIndex = 0u;
				shadowPacked[drawRecordIndex].drawData.modelMatrix =
					histories[drawRecordIndex].currentWorldTransform;
				shadowPacked[drawRecordIndex].drawData.prevModelMatrix =
					histories[drawRecordIndex].previousWorldTransform;
			}
			shadowPacked.erase(shadowPacked.begin() + 1);
			expect(shadowPacked.size() == 2u
			       && shadowPacked[1].drawRecordIndex == 2u,
			       "removing the middle instance discarded the moved shadow identity");

			std::array<std::array<shaderio::DrawUniforms, 6>, 3> framePayloads{};
			const auto publishSubmittedFrame = [&](uint32_t submittedFrame)
			{
				const uint32_t frameSlot = submittedFrame % frameSlotCount;
				framePayloads[frameSlot] = {};
				for (uint32_t drawIndex = 0; drawIndex < identities.size(); ++drawIndex)
				{
					if (!identities[drawIndex].live)
					{
						continue;
					}
					const demo::SubmittedTransformHistory& history =
						histories[identities[drawIndex].drawRecordIndex];
					framePayloads[frameSlot][drawIndex].modelMatrix = history.currentWorldTransform;
					framePayloads[frameSlot][drawIndex].prevModelMatrix = history.previousWorldTransform;
				}
				return frameSlot;
			};
			const auto expectMovedPayload = [&](uint32_t frameSlot,
			                                    const glm::mat4& expectedPrevious,
			                                    const glm::mat4& expectedCurrent)
			{
				for (uint32_t drawIndex : std::array<uint32_t, 2>{4u, 5u})
				{
					expect(identities[drawIndex].sceneObjectIndex == 1u
					       && meshletObjectIndices[drawIndex] == 1u,
					       "moved meshlet retained its pre-compaction scene object index");
					expect(matricesNearlyEqual(
						framePayloads[frameSlot][drawIndex].prevModelMatrix, expectedPrevious, 1.0e-6f)
					       && matricesNearlyEqual(
						       framePayloads[frameSlot][drawIndex].modelMatrix, expectedCurrent, 1.0e-6f),
					       "moved meshlet drawData diverged after dense remap");
				}
				expect(meshletObjectIndices[2] == UINT32_MAX && meshletIndexCounts[2] == 0u
				       && meshletObjectIndices[3] == UINT32_MAX && meshletIndexCounts[3] == 0u,
				       "removed meshlet draw entries were not safely tombstoned");
			};

			demo::updateSubmittedTransformHistory(histories[2], movedFrameZero);
			demo::updateShadowPackedMeshRuntimeState(
				shadowPacked[1],
				histories[2].currentWorldTransform,
				histories[2].previousWorldTransform,
				glm::vec4(glm::vec3(movedFrameZero * glm::vec4(0.3f, -0.2f, 0.6f, 1.0f)), 6.0f));
			const uint32_t frameZeroSlot = publishSubmittedFrame(0u);
			expectMovedPayload(frameZeroSlot, movedInitial, movedFrameZero);
			expect(matricesNearlyEqual(shadowPacked[1].drawData.prevModelMatrix, movedInitial, 1.0e-6f)
			       && matricesNearlyEqual(shadowPacked[1].drawData.modelMatrix, movedFrameZero, 1.0e-6f),
			       "shadowPacked diverged from remapped raster payload on first submitted frame");
			demo::commitSubmittedTransformHistory(histories[2]);
			demo::commitShadowPackedMeshSubmittedState(shadowPacked[1]);

			const uint32_t frameOneSlot = publishSubmittedFrame(1u);
			expectMovedPayload(frameOneSlot, movedFrameZero, movedFrameZero);

			demo::updateSubmittedTransformHistory(histories[2], movedFrameTwo);
			demo::updateShadowPackedMeshRuntimeState(
				shadowPacked[1],
				histories[2].currentWorldTransform,
				histories[2].previousWorldTransform,
				glm::vec4(glm::vec3(movedFrameTwo * glm::vec4(0.3f, -0.2f, 0.6f, 1.0f)), 6.0f));
			const uint32_t frameTwoSlot = publishSubmittedFrame(2u);
			expect(frameTwoSlot == (frameSlotCount == 2u ? 0u : 2u),
			       "submitted frame did not wrap through the configured frame-slot ring");
			expectMovedPayload(frameTwoSlot, movedFrameZero, movedFrameTwo);
			expect(matricesNearlyEqual(shadowPacked[1].drawData.prevModelMatrix, movedFrameZero, 1.0e-6f)
			       && matricesNearlyEqual(shadowPacked[1].drawData.modelMatrix, movedFrameTwo, 1.0e-6f),
			       "shadowPacked diverged after dense remap on the third submitted frame");
		}
	}
	void testLargeNormalBiasExpandsCascadeCullingGuard()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(6.0f, 8.0f, 14.0f), glm::normalize(glm::vec3(-0.32f, -0.21f, -0.92f)));
		const glm::vec3 lightDirection = glm::normalize(glm::vec3(-0.35f, -0.8f, -0.48f));
		constexpr float kLargeNormalBiasWorld = 12.0f;

		demo::CSMShadowResources baselineCsm;
		updateProductionCascades(baselineCsm, camera, lightDirection, 0.0f);
		const auto baselineFrame = baselineCsm.getFrameData();
		demo::CSMShadowResources biasedCsm;
		updateProductionCascades(biasedCsm, camera, lightDirection, kLargeNormalBiasWorld);
		const auto biasedFrame = biasedCsm.getFrameData();

		expect(nearlyEqual(biasedFrame.normalBiasWorld, kLargeNormalBiasWorld, 1.0e-6f),
		       "CSM frame data did not publish the actual caster normalBias");
		for (uint32_t cascadeIndex = 0; cascadeIndex < baselineFrame.cascadeCount; ++cascadeIndex)
		{
			const auto& baseline = baselineFrame.cascades[cascadeIndex];
			const auto& biased = biasedFrame.cascades[cascadeIndex];
			expect(biased.cullingGuardWorld >= kLargeNormalBiasWorld,
			       "cascade XY guard is smaller than the caster normalBias world offset");
			expect(matricesNearlyEqual(baseline.lightView, biased.lightView, 1.0e-6f)
			       && matricesNearlyEqual(baseline.lightProjection, biased.lightProjection, 1.0e-6f)
			       && matricesNearlyEqual(baseline.viewProjection, biased.viewProjection, 1.0e-6f),
			       "normalBias changed the rendered cascade fit instead of only culling guard");
			expect(nearlyEqual(baseline.depthRange, biased.depthRange, 1.0e-6f),
			       "normalBias changed the stable caster depth span");

			const float expectedPlaneExpansion = biased.cullingGuardWorld - baseline.cullingGuardWorld;
			expect(expectedPlaneExpansion > 0.0f,
			       "large normalBias did not enlarge the cascade culling footprint");
			for (uint32_t planeIndex = 0; planeIndex < 4u; ++planeIndex)
			{
				expect(vectorsNearlyEqual(glm::vec3(baseline.cullingPlanes[planeIndex]),
				                          glm::vec3(biased.cullingPlanes[planeIndex]), 1.0e-6f),
				       "normalBias changed a cascade culling side-plane orientation");
				expect(nearlyEqual(
					biased.cullingPlanes[planeIndex].w - baseline.cullingPlanes[planeIndex].w,
					expectedPlaneExpansion,
					1.0e-4f),
				       "cascade culling side plane did not include the full normalBias offset");
			}
		}
	}

	void testNormalBiasSanitization()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(6.0f, 8.0f, 14.0f), glm::normalize(glm::vec3(-0.32f, -0.21f, -0.92f)));
		const glm::vec3 lightDirection = glm::normalize(glm::vec3(-0.35f, -0.8f, -0.48f));
		demo::CSMShadowResources baselineCsm;
		updateProductionCascades(baselineCsm, camera, lightDirection, 0.0f);
		const auto baselineFrame = baselineCsm.getFrameData();

		struct NormalBiasCase
		{
			float requested;
			float expected;
		};
		const std::array<NormalBiasCase, 4> cases{{
			{std::numeric_limits<float>::quiet_NaN(), 0.0f},
			{std::numeric_limits<float>::infinity(), 0.0f},
			{-3.0f, 0.0f},
			{2.0f, 2.0f},
		}};

		for (const NormalBiasCase& biasCase : cases)
		{
			demo::CSMShadowResources csm;
			updateProductionCascades(csm, camera, lightDirection, biasCase.requested);
			const auto frame = csm.getFrameData();
			expect(std::isfinite(frame.normalBiasWorld),
			       "CSM frame data retained a non-finite normalBias");
			expect(nearlyEqual(frame.normalBiasWorld, biasCase.expected, 1.0e-6f),
			       "CSM frame data did not clamp normalBias to its sanitized world value");
			for (uint32_t cascadeIndex = 0; cascadeIndex < frame.cascadeCount; ++cascadeIndex)
			{
				const float expectedGuard =
					std::max(baselineFrame.cascades[cascadeIndex].cullingGuardWorld, biasCase.expected);
				expect(std::isfinite(frame.cascades[cascadeIndex].cullingGuardWorld),
				       "non-finite normalBias polluted a cascade culling guard");
				expect(nearlyEqual(
					frame.cascades[cascadeIndex].cullingGuardWorld, expectedGuard, 1.0e-5f),
				       "cascade culling guard did not use the sanitized normalBias value");
			}
		}
	}

	void testSafeLightDirectionSnapshot()
	{
		const shaderio::CameraUniforms camera = makeCamera(
			glm::vec3(2.0f, 5.0f, 9.0f), glm::normalize(glm::vec3(-0.2f, -0.1f, -1.0f)));
		demo::CSMShadowResources csm;

		csm.updateCascadeMatrices(camera, glm::vec3(0.0f), kShadowDistance);
		const glm::vec3 fallbackDirection = csm.getFrameData().lightDirection;
		expect(vectorsNearlyEqual(fallbackDirection, glm::vec3(0.0f, -1.0f, 0.0f), 1.0e-6f),
		       "zero light direction did not use the CSM safe fallback");
		expect(nearlyEqual(glm::length(fallbackDirection), 1.0f, 1.0e-6f),
		       "CSM fallback light direction is not unit length");

		const glm::vec3 unnormalizedDirection(0.0f, -12.0f, 5.0f);
		csm.updateCascadeMatrices(camera, unnormalizedDirection, kShadowDistance);
		expect(vectorsNearlyEqual(csm.getFrameData().lightDirection, glm::normalize(unnormalizedDirection), 1.0e-6f),
		       "CSM frame data did not publish the safely normalized light direction");

		auto evaluate = [&](const glm::vec3& inputDirection)
		{
			demo::CSMShadowResources isolatedCsm;
			isolatedCsm.updateCascadeMatrices(camera, inputDirection, kShadowDistance);
			const auto& frame = isolatedCsm.getFrameData();
			verifyLightFrameAxes(frame.cascades[0].lightView);
			expect(nearlyEqual(glm::length(frame.lightDirection), 1.0f, 1.0e-6f),
			       "finite non-zero light direction did not normalize to unit length");
			return LightDirectionBasisSample{
				frame.lightDirection,
				extractLightFrameAxes(frame.cascades[0].lightView),
			};
		};

		const float lowerMagnitude = std::nextafter(1.0e-4f, 0.0f);
		const float upperMagnitude =
			std::nextafter(1.0e-4f, std::numeric_limits<float>::infinity());
		const LightDirectionBasisSample lowerMagnitudeSample =
			evaluate(glm::vec3(lowerMagnitude, 0.0f, 0.0f));
		const LightDirectionBasisSample upperMagnitudeSample =
			evaluate(glm::vec3(upperMagnitude, 0.0f, 0.0f));
		expect(vectorsNearlyEqual(
			lowerMagnitudeSample.direction, glm::vec3(1.0f, 0.0f, 0.0f), 1.0e-6f)
		       && vectorsNearlyEqual(
			       upperMagnitudeSample.direction, glm::vec3(1.0f, 0.0f, 0.0f), 1.0e-6f),
		       "1e-4 nextafter light directions did not preserve their common direction");
		expect(maxLightBasisAngleRadians(
			lowerMagnitudeSample.axes, upperMagnitudeSample.axes) < 1.0e-5f,
		       "1e-4 nextafter magnitudes changed the normalized light basis");

		const float tinyMagnitude = std::numeric_limits<float>::denorm_min();
		const LightDirectionBasisSample tinySample =
			evaluate(glm::vec3(tinyMagnitude, 0.0f, 0.0f));
		expect(vectorsNearlyEqual(tinySample.direction, upperMagnitudeSample.direction, 1.0e-6f),
		       "smallest finite non-zero light direction fell back instead of normalizing");
		expect(maxLightBasisAngleRadians(tinySample.axes, upperMagnitudeSample.axes) < 1.0e-5f,
		       "smallest finite non-zero light direction changed the normalized light basis");

		const float maximumFinite = std::numeric_limits<float>::max();
		const glm::vec3 hugeDirection(
			maximumFinite, -maximumFinite * 0.5f, maximumFinite * 0.25f);
		const glm::vec3 moderateDirection(1.0f, -0.5f, 0.25f);
		const LightDirectionBasisSample hugeSample = evaluate(hugeDirection);
		const LightDirectionBasisSample moderateSample = evaluate(moderateDirection);
		expect(vectorsNearlyEqual(hugeSample.direction, moderateSample.direction, 1.0e-6f),
		       "large finite light direction overflowed instead of scale-safe normalization");
		expect(maxLightBasisAngleRadians(hugeSample.axes, moderateSample.axes) < 1.0e-5f,
		       "large finite light direction changed the scale-invariant light basis");
	}

	void testShadowTextureTransformIsBackendIndependent()
	{
		const glm::mat4 vulkanTransform = demo::clipspace::makeNdcToShadowTextureMatrix(
			demo::clipspace::getProjectionConvention(demo::clipspace::BackendConvention::vulkan));
		const glm::mat4 d3d12Transform = demo::clipspace::makeNdcToShadowTextureMatrix(
			demo::clipspace::getProjectionConvention(demo::clipspace::BackendConvention::d3d12));
		const glm::mat4 metalTransform = demo::clipspace::makeNdcToShadowTextureMatrix(
			demo::clipspace::getProjectionConvention(demo::clipspace::BackendConvention::metal));

		expect(matricesNearlyEqual(vulkanTransform, d3d12Transform, 0.0f),
		       "D3D12 shadow texture transform changed backend-independent texture coordinates");
		expect(matricesNearlyEqual(vulkanTransform, metalTransform, 0.0f),
		       "Metal shadow texture transform changed backend-independent texture coordinates");
		expect(nearlyEqual(vulkanTransform[1][1], 0.5f, 0.0f),
		       "shadow texture transform must map NDC Y with a positive half scale");
	}
	void testTinyCascadeResolutionClamp()
	{
		constexpr uint32_t minimumResolution =
			static_cast<uint32_t>(2 * shaderio::LCascadePcfGuardTexels + 1);
		static_assert(demo::CSMShadowResources::getMinimumCascadeResolution() == minimumResolution);
		expect(demo::CSMShadowResources::clampCascadeResolution(0u) == minimumResolution,
		       "zero cascade resolution did not preserve the PCF guard");
		expect(demo::CSMShadowResources::clampCascadeResolution(1u) == minimumResolution,
		       "tiny cascade resolution did not preserve the PCF guard");
		expect(demo::CSMShadowResources::clampCascadeResolution(minimumResolution) == minimumResolution,
		       "minimum valid cascade resolution was changed");
		expect(demo::CSMShadowResources::clampCascadeResolution(minimumResolution + 7u) == minimumResolution + 7u,
		       "valid cascade resolution was unnecessarily changed");
	}
} // namespace

int main()
{
	try
	{
		testCasterNormalOffsetDirectionAndMagnitude();
		testInverseTransposeNormalMatrix();
		testStableLightBasisAcrossVerticalThreshold();
		testPersistentLightBasisAcrossSouthPoleSeam();
		testPersistentLightBasisAcrossAntipodalDirection();
		testNearAntipodalBoundaryPairIsContinuous();
		testLightBasisContinuouslyApproachesAntipode();
		testLightBasisContinuousAcrossFormerAntipodalThresholdSweep();
		testProjectionReliabilityBoundaryIsContinuousInFloat32();
		testDirectionDotZeroCrossingNearProjectionSingularityIsContinuous();
		testAnalyticChartsHandleExactPreviousUpDirections();
		testPersistedProjectionChartCrossesPreviousUpContinuously();
		testTransportChartOneUlpTargetMapIsContinuous();
		testAntipodalChartNearbyTargetMapIsContinuous();
		testComplementaryChartsHandleExactPoles();
		testScaleInvariantPoleReproductions();
		testComponentUlpPoleBoundaryIsRegularized();
		testScaleInvariantPoleScaleSweep();
		testRegularizationAnnulusBoundariesAreSmooth();
		testRegularizationRandomNonAxisHistories();
		testOpposedChartTeleportPerturbationDoesNotFlipRoll();
		testOpposedChartLargeTeleportNeighborhoodScan();
		testLightBasisRandomSmallStepSpherePath();
		testLightBasisClosedGreatCircleScan();
		testRuntimeCasterShadowPackedState();
		testSharedMeshInstanceTransformHistoryAcrossFrameSlots();
		testMeshletDenseRemapAcrossTwoAndThreeFrameSlots();
		testReceiverDrivenCascadeCullingFootprint();
		testLargeNormalBiasExpandsCascadeCullingGuard();
		testNormalBiasSanitization();
		testSafeLightDirectionSnapshot();
		testShadowTextureTransformIsBackendIndependent();
		testTinyCascadeResolutionClamp();
		testStableCascadeProjection();
		testOffCenterProjection();
		testOffCenterOrthographicProjection();
		testLastMovingToSettledTemporalSequence();
		testManySmallTemporalMotionSteps();
		testStableDepthRangeBiasAndGuard();
		testUnjitteredShadowFitCameraConstruction();
		testPreparedCascadesSurviveTemporalJitter();
		constexpr float kRadiansToDegrees = 57.2957795130823208768f;
		std::cout << "CSM basis continuity max adjacent angles (degrees): singularity="
		          << g_maxProjectionSingularityAdjacentAngleRadians * kRadiansToDegrees
		          << ", teleport_pair=" << g_maxChartOverlapTeleportPairAngleRadians * kRadiansToDegrees
		          << ", teleport_scan=" << g_maxLargeTeleportNeighborhoodAdjacentAngleRadians * kRadiansToDegrees
		          << ", random=" << g_maxRandomSpherePathAdjacentAngleRadians * kRadiansToDegrees
		          << ", closed_loop=" << g_maxClosedLoopAdjacentAngleRadians * kRadiansToDegrees
		          << ", zero_projection_analytic="
		          << g_maxZeroProjectionAnalyticAngleRadians * kRadiansToDegrees
		          << ", regularization_component_ulp="
		          << g_maxRegularizationComponentUlpAngleRadians * kRadiansToDegrees
		          << ", regularization_radial=" << g_maxRegularizationBoundaryRadialAngleRadians * kRadiansToDegrees
		          << ", regularization_azimuth=" << g_maxRegularizationBoundaryAzimuthAngleRadians * kRadiansToDegrees
		          << ", regularization_random_ulp=" << g_maxRegularizationRandomUlpAngleRadians * kRadiansToDegrees << '\n';
		std::cout << "CSM shadow stability tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "CSM shadow stability test failed: " << error.what() << '\n';
		return 1;
	}
}
