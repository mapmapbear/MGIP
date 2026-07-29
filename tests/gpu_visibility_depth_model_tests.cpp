#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
	struct DepthImage
	{
		uint32_t width{0};
		uint32_t height{0};
		std::vector<float> pixels;

		float& at(uint32_t x, uint32_t y)
		{
			return pixels[static_cast<size_t>(y) * width + x];
		}

		float at(uint32_t x, uint32_t y) const
		{
			return pixels[static_cast<size_t>(y) * width + x];
		}
	};

	DepthImage makeDepthImage(uint32_t width, uint32_t height, float value)
	{
		return DepthImage{
			.width = width,
			.height = height,
			.pixels = std::vector<float>(static_cast<size_t>(width) * height, value),
		};
	}

	DepthImage reduceSourceDepthToPyramidMip0(const DepthImage& source)
	{
		DepthImage result = makeDepthImage(
			std::max(1u, (source.width + 1u) / 2u),
			std::max(1u, (source.height + 1u) / 2u),
			0.0f);

		for (uint32_t y = 0; y < result.height; ++y)
		{
			for (uint32_t x = 0; x < result.width; ++x)
			{
				float farthestDepth = 1.0f;
				for (uint32_t dy = 0; dy < 2u; ++dy)
				{
					for (uint32_t dx = 0; dx < 2u; ++dx)
					{
						const uint32_t sourceX = x * 2u + dx;
						const uint32_t sourceY = y * 2u + dy;
						const float sample =
							sourceX < source.width && sourceY < source.height
								? source.at(sourceX, sourceY)
								: 0.0f; // Reverse-Z far/empty outside the screen.
						farthestDepth = std::min(farthestDepth, sample);
					}
				}
				result.at(x, y) = farthestDepth;
			}
		}
		return result;
	}

	uint32_t vulkanMipDimension(uint32_t baseDimension, uint32_t mipLevel)
	{
		uint32_t dimension = std::max(baseDimension, 1u);
		for (uint32_t level = 0u; level < mipLevel && dimension > 1u; ++level)
		{
			dimension >>= 1u;
		}
		return dimension;
	}

	DepthImage reduceVulkanPyramidMipMin2x2(const DepthImage& source)
	{
		DepthImage result = makeDepthImage(
			vulkanMipDimension(source.width, 1u),
			vulkanMipDimension(source.height, 1u),
			0.0f);

		for (uint32_t y = 0; y < result.height; ++y)
		{
			for (uint32_t x = 0; x < result.width; ++x)
			{
				float farthestDepth = 1.0f;
				for (uint32_t dy = 0; dy < 2u; ++dy)
				{
					for (uint32_t dx = 0; dx < 2u; ++dx)
					{
						const uint32_t sourceX = x * 2u + dx;
						const uint32_t sourceY = y * 2u + dy;
						const float sample =
							sourceX < source.width && sourceY < source.height
								? source.at(sourceX, sourceY)
								: 0.0f;
						farthestDepth = std::min(farthestDepth, sample);
					}
				}
				result.at(x, y) = farthestDepth;
			}
		}
		return result;
	}

	float representedSourcePixelMax(uint32_t screenDimension,
	                                uint32_t pyramidDimension,
	                                uint32_t mipLevel)
	{
		const uint32_t sourceTexelSize = 1u << (mipLevel + 1u);
		const uint32_t mipDimension = vulkanMipDimension(pyramidDimension, mipLevel);
		return static_cast<float>(
			std::min(screenDimension, mipDimension * sourceTexelSize));
	}

	bool isFootprintDimensionRepresented(float pixelMin,
	                                     float pixelMax,
	                                     uint32_t screenDimension,
	                                     uint32_t pyramidDimension,
	                                     uint32_t mipLevel)
	{
		return pixelMin >= 0.0f
			&& pixelMax <= representedSourcePixelMax(
				screenDimension, pyramidDimension, mipLevel) + 1.0e-4f;
	}

	float sampleCompleteFootprintMin(const DepthImage& mip,
	                                 uint32_t minX,
	                                 uint32_t minY,
	                                 uint32_t maxX,
	                                 uint32_t maxY)
	{
		float farthestDepth = 1.0f;
		for (uint32_t y = minY; y <= maxY; ++y)
		{
			for (uint32_t x = minX; x <= maxX; ++x)
			{
				farthestDepth = std::min(farthestDepth, mip.at(x, y));
			}
		}
		return farthestDepth;
	}

	bool isConservativelyOccluded(const DepthImage& mip,
	                              uint32_t minX,
	                              uint32_t minY,
	                              uint32_t maxX,
	                              uint32_t maxY,
	                              float objectNearestDepth,
	                              float epsilon = 2.0e-3f)
	{
		const float occluderFarthestDepth =
			sampleCompleteFootprintMin(mip, minX, minY, maxX, maxY);
		if (occluderFarthestDepth <= epsilon)
		{
			return false;
		}
		return objectNearestDepth + epsilon < occluderFarthestDepth;
	}

	bool alphaMaskWritesDepth(float baseColorFactorAlpha,
	                          bool hasBaseColorTexture,
	                          float textureAlpha,
	                          float alphaCutoff)
	{
		const float alpha =
			baseColorFactorAlpha * (hasBaseColorTexture ? textureAlpha : 1.0f);
		return alpha >= alphaCutoff;
	}

	struct ModelFloat3
	{
		float x{0.0f};
		float y{0.0f};
		float z{0.0f};
	};

	struct ModelMatrix3
	{
		std::array<ModelFloat3, 3> rows{};
	};

	float dot3(const ModelFloat3& lhs, const ModelFloat3& rhs)
	{
		return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
	}

	ModelFloat3 cross3(const ModelFloat3& lhs, const ModelFloat3& rhs)
	{
		return ModelFloat3{
			.x = lhs.y * rhs.z - lhs.z * rhs.y,
			.y = lhs.z * rhs.x - lhs.x * rhs.z,
			.z = lhs.x * rhs.y - lhs.y * rhs.x,
		};
	}

	float length3(const ModelFloat3& value)
	{
		return std::sqrt(dot3(value, value));
	}

	ModelFloat3 normalize3(const ModelFloat3& value)
	{
		const float valueLength = length3(value);
		if (!std::isfinite(valueLength) || valueLength <= 1.0e-10f)
		{
			return {};
		}
		return ModelFloat3{
			.x = value.x / valueLength,
			.y = value.y / valueLength,
			.z = value.z / valueLength,
		};
	}

	ModelFloat3 multiply(const ModelMatrix3& matrix, const ModelFloat3& vector)
	{
		return ModelFloat3{
			.x = dot3(matrix.rows[0], vector),
			.y = dot3(matrix.rows[1], vector),
			.z = dot3(matrix.rows[2], vector),
		};
	}

	bool isFinite(const ModelFloat3& value)
	{
		return std::isfinite(value.x)
			&& std::isfinite(value.y)
			&& std::isfinite(value.z);
	}

	float legacyMaxColumnRadiusScale(const ModelMatrix3& matrix)
	{
		const ModelFloat3 column0{
			.x = matrix.rows[0].x,
			.y = matrix.rows[1].x,
			.z = matrix.rows[2].x,
		};
		const ModelFloat3 column1{
			.x = matrix.rows[0].y,
			.y = matrix.rows[1].y,
			.z = matrix.rows[2].y,
		};
		const ModelFloat3 column2{
			.x = matrix.rows[0].z,
			.y = matrix.rows[1].z,
			.z = matrix.rows[2].z,
		};
		return std::max({length3(column0), length3(column1), length3(column2)});
	}

	float frobeniusRadiusScale(const ModelMatrix3& matrix)
	{
		return std::sqrt(
			dot3(matrix.rows[0], matrix.rows[0])
			+ dot3(matrix.rows[1], matrix.rows[1])
			+ dot3(matrix.rows[2], matrix.rows[2]));
	}

	bool isSimilarityTransformForCone(const ModelMatrix3& matrix)
	{
		if (!isFinite(matrix.rows[0])
			|| !isFinite(matrix.rows[1])
			|| !isFinite(matrix.rows[2]))
		{
			return false;
		}

		const float rowLengthSq0 = dot3(matrix.rows[0], matrix.rows[0]);
		const float rowLengthSq1 = dot3(matrix.rows[1], matrix.rows[1]);
		const float rowLengthSq2 = dot3(matrix.rows[2], matrix.rows[2]);
		const float maxRowLengthSq =
			std::max({rowLengthSq0, rowLengthSq1, rowLengthSq2});
		const float minRowLengthSq =
			std::min({rowLengthSq0, rowLengthSq1, rowLengthSq2});
		if (!std::isfinite(maxRowLengthSq) || minRowLengthSq <= 1.0e-10f)
		{
			return false;
		}

		const float gramTolerance = maxRowLengthSq * 1.0e-6f;
		if (maxRowLengthSq - minRowLengthSq > gramTolerance
			|| std::abs(dot3(matrix.rows[0], matrix.rows[1])) > gramTolerance
			|| std::abs(dot3(matrix.rows[0], matrix.rows[2])) > gramTolerance
			|| std::abs(dot3(matrix.rows[1], matrix.rows[2])) > gramTolerance)
		{
			return false;
		}

		const float determinant =
			dot3(matrix.rows[0], cross3(matrix.rows[1], matrix.rows[2]));
		const float determinantTolerance =
			maxRowLengthSq * std::sqrt(maxRowLengthSq) * 1.0e-6f;
		return std::isfinite(determinant)
			&& determinant > determinantTolerance;
	}

	bool tryInverseTransposeNormalAxis(const ModelMatrix3& matrix,
	                                   const ModelFloat3& localAxis,
	                                   ModelFloat3& worldAxis)
	{
		worldAxis = {};
		if (!isFinite(localAxis)
			|| !isFinite(matrix.rows[0])
			|| !isFinite(matrix.rows[1])
			|| !isFinite(matrix.rows[2]))
		{
			return false;
		}

		const ModelFloat3 cofactorRow0 = cross3(matrix.rows[1], matrix.rows[2]);
		const ModelFloat3 cofactorRow1 = cross3(matrix.rows[2], matrix.rows[0]);
		const ModelFloat3 cofactorRow2 = cross3(matrix.rows[0], matrix.rows[1]);
		const float determinant = dot3(matrix.rows[0], cofactorRow0);
		const float maxRowLengthSq = std::max({
			dot3(matrix.rows[0], matrix.rows[0]),
			dot3(matrix.rows[1], matrix.rows[1]),
			dot3(matrix.rows[2], matrix.rows[2]),
		});
		if (!std::isfinite(maxRowLengthSq))
		{
			return false;
		}
		const float determinantTolerance =
			maxRowLengthSq * std::sqrt(maxRowLengthSq) * 1.0e-6f;
		if (!std::isfinite(determinant) || determinant <= determinantTolerance)
		{
			return false;
		}

		worldAxis = normalize3(ModelFloat3{
			.x = dot3(cofactorRow0, localAxis),
			.y = dot3(cofactorRow1, localAxis),
			.z = dot3(cofactorRow2, localAxis),
		});
		return isFinite(worldAxis) && dot3(worldAxis, worldAxis) > 0.0f;
	}

	bool tryTransformConeAxisForCulling(const ModelMatrix3& matrix,
	                                    const ModelFloat3& localAxis,
	                                    ModelFloat3& worldAxis)
	{
		if (!isSimilarityTransformForCone(matrix))
		{
			worldAxis = {};
			return false;
		}
		return tryInverseTransposeNormalAxis(matrix, localAxis, worldAxis);
	}

	struct ModelFloat4
	{
		float x{0.0f};
		float y{0.0f};
		float z{0.0f};
		float w{0.0f};
	};

	struct ModelMatrix4
	{
		std::array<std::array<float, 4>, 4> rows{};
	};

	ModelFloat4 multiply(const ModelMatrix4& matrix, const ModelFloat4& vector)
	{
		ModelFloat4 result{};
		float* resultValues[4] = {&result.x, &result.y, &result.z, &result.w};
		const float vectorValues[4] = {vector.x, vector.y, vector.z, vector.w};
		for (uint32_t row = 0u; row < 4u; ++row)
		{
			for (uint32_t column = 0u; column < 4u; ++column)
			{
				*resultValues[row] += matrix.rows[row][column] * vectorValues[column];
			}
		}
		return result;
	}

	ModelMatrix4 makeReverseZPerspective(float xScale,
	                                     float yScale,
	                                     float xOffset,
	                                     float yOffset,
	                                     float nearPlane,
	                                     float farPlane)
	{
		ModelMatrix4 result{};
		const float depthScale = nearPlane / (farPlane - nearPlane);
		const float depthOffset = farPlane * nearPlane / (farPlane - nearPlane);
		result.rows[0] = {xScale, 0.0f, xOffset, 0.0f};
		result.rows[1] = {0.0f, yScale, yOffset, 0.0f};
		result.rows[2] = {0.0f, 0.0f, depthScale, depthOffset};
		result.rows[3] = {0.0f, 0.0f, -1.0f, 0.0f};
		return result;
	}

	ModelMatrix4 makeReverseZOrthographic(float left,
	                                      float right,
	                                      float bottom,
	                                      float top,
	                                      float nearPlane,
	                                      float farPlane)
	{
		ModelMatrix4 result{};
		result.rows[0] = {
			2.0f / (right - left), 0.0f, 0.0f, -(right + left) / (right - left)};
		result.rows[1] = {
			0.0f, -2.0f / (top - bottom), 0.0f, (top + bottom) / (top - bottom)};
		result.rows[2] = {
			0.0f, 0.0f, 1.0f / (farPlane - nearPlane),
			farPlane / (farPlane - nearPlane)};
		result.rows[3] = {0.0f, 0.0f, 0.0f, 1.0f};
		return result;
	}

	struct ProjectedSphereBounds
	{
		bool valid{false};
		float minX{0.0f};
		float minY{0.0f};
		float maxX{0.0f};
		float maxY{0.0f};
		float nearestDepth{0.0f};
	};

	bool projectPointToNdc(const ModelMatrix4& projection,
	                       const ModelFloat3& point,
	                       ModelFloat3& ndc)
	{
		const ModelFloat4 clip = multiply(
			projection, ModelFloat4{.x = point.x, .y = point.y, .z = point.z, .w = 1.0f});
		const float tolerance = 1.0e-5f * std::max(1.0f, std::abs(clip.w));
		if (clip.w <= tolerance || clip.z < -tolerance || clip.z > clip.w + tolerance)
		{
			return false;
		}

		ndc = ModelFloat3{
			.x = clip.x / clip.w,
			.y = clip.y / clip.w,
			.z = std::clamp(clip.z / clip.w, 0.0f, 1.0f),
		};
		return std::isfinite(ndc.x) && std::isfinite(ndc.y) && std::isfinite(ndc.z);
	}

	ProjectedSphereBounds projectConservativeSphereAabb(
		const ModelMatrix4& projection, const ModelFloat3& center, float radius)
	{
		ProjectedSphereBounds result{
			.valid = true,
			.minX = 1.0e30f,
			.minY = 1.0e30f,
			.maxX = -1.0e30f,
			.maxY = -1.0e30f,
			.nearestDepth = 0.0f,
		};

		for (uint32_t cornerIndex = 0u; cornerIndex < 8u; ++cornerIndex)
		{
			const ModelFloat3 corner{
				.x = center.x + ((cornerIndex & 1u) != 0u ? radius : -radius),
				.y = center.y + ((cornerIndex & 2u) != 0u ? radius : -radius),
				.z = center.z + ((cornerIndex & 4u) != 0u ? radius : -radius),
			};
			ModelFloat3 ndc{};
			if (!projectPointToNdc(projection, corner, ndc))
			{
				return {};
			}

			result.minX = std::min(result.minX, ndc.x);
			result.minY = std::min(result.minY, ndc.y);
			result.maxX = std::max(result.maxX, ndc.x);
			result.maxY = std::max(result.maxY, ndc.y);
			result.nearestDepth = std::max(result.nearestDepth, ndc.z);
		}
		return result;
	}

	bool projectedSphereSamplesFit(const ModelMatrix4& projection,
	                               const ModelFloat3& center,
	                               float radius,
	                               const ProjectedSphereBounds& bounds)
	{
		if (!bounds.valid)
		{
			return false;
		}

		constexpr float pi = 3.14159265358979323846f;
		for (uint32_t latitude = 0u; latitude <= 64u; ++latitude)
		{
			const float polar = pi * static_cast<float>(latitude) / 64.0f;
			const float sinPolar = std::sin(polar);
			const float cosPolar = std::cos(polar);
			for (uint32_t longitude = 0u; longitude < 128u; ++longitude)
			{
				const float azimuth =
					2.0f * pi * static_cast<float>(longitude) / 128.0f;
				const ModelFloat3 point{
					.x = center.x + radius * sinPolar * std::cos(azimuth),
					.y = center.y + radius * cosPolar,
					.z = center.z + radius * sinPolar * std::sin(azimuth),
				};
				ModelFloat3 ndc{};
				if (!projectPointToNdc(projection, point, ndc)
					|| ndc.x < bounds.minX - 1.0e-4f
					|| ndc.y < bounds.minY - 1.0e-4f
					|| ndc.x > bounds.maxX + 1.0e-4f
					|| ndc.y > bounds.maxY + 1.0e-4f
					|| ndc.z > bounds.nearestDepth + 1.0e-4f)
				{
					return false;
				}
			}
		}
		return true;
	}

	bool perspectiveSphereExceedsNaiveCenterRadiusBound(
		const ModelMatrix4& projection, const ModelFloat3& center, float radius)
	{
		ModelFloat3 centerNdc{};
		if (!projectPointToNdc(projection, center, centerNdc))
		{
			return false;
		}

		const float nearestPositiveZ = -center.z - radius;
		const float naiveMaxX =
			centerNdc.x + std::abs(projection.rows[0][0]) * radius / nearestPositiveZ;
		constexpr float pi = 3.14159265358979323846f;
		for (uint32_t sampleIndex = 0u; sampleIndex < 4096u; ++sampleIndex)
		{
			const float angle =
				2.0f * pi * static_cast<float>(sampleIndex) / 4096.0f;
			const ModelFloat3 point{
				.x = center.x + radius * std::cos(angle),
				.y = center.y,
				.z = center.z + radius * std::sin(angle),
			};
			ModelFloat3 ndc{};
			if (projectPointToNdc(projection, point, ndc)
				&& ndc.x > naiveMaxX + 1.0e-4f)
			{
				return true;
			}
		}
		return false;
	}

	float rebuildCurrentVisibleDepth(std::initializer_list<float> currentVisibleDepths)
	{
		float depth = 0.0f; // Reverse-Z far clear.
		for (const float candidate : currentVisibleDepths)
		{
			if (candidate >= depth)
			{
				depth = candidate;
			}
		}
		return depth;
	}

	float loadBootstrapThenPatch(float bootstrapDepth,
	                             std::initializer_list<float> currentVisibleDepths)
	{
		float depth = bootstrapDepth;
		for (const float candidate : currentVisibleDepths)
		{
			if (candidate >= depth)
			{
				depth = candidate;
			}
		}
		return depth;
	}

	struct VisibilityPatchScanModel
	{
		uint32_t scanStepCount{0};
		uint32_t finalPrefixBufferIndex{0};
		bool prefixBufferRead[2]{false, false};
		uint32_t producerReadAfterWriteBarrierCount{0};
		uint32_t categoryReuseWriteAfterReadBarrierCount{0};
		uint32_t finalIndirectReadBarrierCount{0};
	};

	VisibilityPatchScanModel modelVisibilityPatchScan(uint32_t paddedElementCount)
	{
		VisibilityPatchScanModel model{};
		uint32_t scanBufferIndex = 0u;

		// Mode 0 initializes prefix A. Each producer must become visible to the
		// next compute consumer, including the zero-scan-step final dispatch.
		++model.producerReadAfterWriteBarrierCount;
		for (uint32_t scanOffset = 1u; scanOffset < paddedElementCount; scanOffset <<= 1u)
		{
			model.prefixBufferRead[scanBufferIndex] = true;
			++model.scanStepCount;
			scanBufferIndex = 1u - scanBufferIndex;
			++model.producerReadAfterWriteBarrierCount;
		}

		// Mode 2 reads whichever ping-pong buffer contains the final scan.
		model.finalPrefixBufferIndex = scanBufferIndex;
		model.prefixBufferRead[scanBufferIndex] = true;

		// A following category overwrites prefix A, and draws consume the final
		// patched indirect/count outputs.
		model.categoryReuseWriteAfterReadBarrierCount = 1u;
		model.finalIndirectReadBarrierCount = 1u;
		return model;
	}

	uint32_t visibilitySortPaddedElementCount(uint32_t activeElementCount)
	{
		if (activeElementCount == 0u)
		{
			return 0u;
		}

		uint32_t paddedElementCount = 1u;
		while (paddedElementCount < activeElementCount)
		{
			paddedElementCount <<= 1u;
		}
		return paddedElementCount;
	}

	struct VisibilitySortRecordingModel
	{
		uint32_t activeElementCount{0};
		uint32_t paddedElementCount{0};
		uint32_t keyCopyCount{0};
		uint32_t valueCopyCount{0};
		uint32_t transferToComputeBarrierCount{0};
		uint32_t bitonicDispatchCount{0};
	};

	VisibilitySortRecordingModel modelVisibilitySortRecording(uint32_t activeElementCount)
	{
		VisibilitySortRecordingModel model{
			.activeElementCount = activeElementCount,
			.paddedElementCount = visibilitySortPaddedElementCount(activeElementCount),
		};
		if (model.paddedElementCount == 0u)
		{
			return model;
		}

		// The upload path always copies both key and value arrays. A singleton only
		// skips the bitonic dispatches; its transfer writes still feed patch compute.
		model.keyCopyCount = 1u;
		model.valueCopyCount = 1u;
		model.transferToComputeBarrierCount = 1u;
		for (uint32_t level = 2u; level <= model.paddedElementCount; level <<= 1u)
		{
			for (uint32_t levelMask = level >> 1u; levelMask > 0u; levelMask >>= 1u)
			{
				++model.bitonicDispatchCount;
			}
		}
		return model;
	}

	enum class TombstoneCullCategory : uint8_t
	{
		opaque,
		alphaTest,
		transparent,
	};

	struct TombstoneCullInput
	{
		bool tombstone{false};
		bool visible{false};
		TombstoneCullCategory category{TombstoneCullCategory::opaque};
		uint32_t indexCount{3u};
	};

	struct TombstoneIndirectCommand
	{
		uint32_t indexCount{0u};
		uint32_t instanceCount{0u};
		uint32_t firstIndex{0u};
		int32_t vertexOffset{0};
		uint32_t firstInstance{0u};
	};

	bool isZeroCommand(const TombstoneIndirectCommand& command)
	{
		return command.indexCount == 0u
			&& command.instanceCount == 0u
			&& command.firstIndex == 0u
			&& command.vertexOffset == 0
			&& command.firstInstance == 0u;
	}

	TombstoneIndirectCommand staleIndirectCommand()
	{
		return TombstoneIndirectCommand{
			.indexCount = 997u,
			.instanceCount = 1u,
			.firstIndex = 991u,
			.vertexOffset = 983,
			.firstInstance = 977u,
		};
	}

	struct TombstoneCullModel
	{
		std::vector<TombstoneIndirectCommand> rawCommands;
		uint32_t totalCount{0u};
		uint32_t opaqueCount{0u};
		uint32_t transparentCount{0u};
		uint32_t visibleCount{0u};
		uint32_t opaqueDrawCount{0u};
		uint32_t alphaTestDrawCount{0u};
		uint32_t transparentDrawCount{0u};
		uint32_t totalDrawCount{0u};
	};

	TombstoneCullModel modelTombstoneCull(
		const std::vector<TombstoneCullInput>& inputs)
	{
		TombstoneCullModel model{};
		model.rawCommands.assign(inputs.size(), staleIndirectCommand());
		for (uint32_t drawIndex = 0u;
		     drawIndex < static_cast<uint32_t>(inputs.size());
		     ++drawIndex)
		{
			const TombstoneCullInput& input = inputs[drawIndex];
			if (input.tombstone)
			{
				// Mirrors the shader's first branch: clear a potentially stale raw
				// indirect slot and return before classification or any counter update.
				model.rawCommands[drawIndex] = {};
				continue;
			}

			++model.totalCount;
			if (input.category == TombstoneCullCategory::transparent)
			{
				++model.transparentCount;
			}
			else
			{
				++model.opaqueCount;
			}

			TombstoneIndirectCommand command{
				.indexCount = input.indexCount,
				.instanceCount = 0u,
				.firstIndex = drawIndex * 3u,
				.vertexOffset = static_cast<int32_t>(drawIndex),
				.firstInstance = drawIndex,
			};
			model.rawCommands[drawIndex] = command;
			if (!input.visible)
			{
				continue;
			}

			command.instanceCount = 1u;
			model.rawCommands[drawIndex] = command;
			++model.visibleCount;
			switch (input.category)
			{
				case TombstoneCullCategory::opaque:
					++model.opaqueDrawCount;
					break;
				case TombstoneCullCategory::alphaTest:
					++model.alphaTestDrawCount;
					break;
				case TombstoneCullCategory::transparent:
					++model.transparentDrawCount;
					break;
			}
			++model.totalDrawCount;
		}
		return model;
	}

	struct TombstoneVisibilityPatchModel
	{
		std::vector<TombstoneIndirectCommand> targetCommands;
		uint32_t patchedCommandCount{0u};
	};

	TombstoneVisibilityPatchModel modelTombstoneVisibilityPatch(
		const TombstoneCullModel& culling,
		const std::vector<uint32_t>& sortedDrawIndices,
		uint32_t outputCapacity)
	{
		TombstoneVisibilityPatchModel patch{};
		patch.targetCommands.assign(outputCapacity, staleIndirectCommand());
		for (const uint32_t drawIndex : sortedDrawIndices)
		{
			if (drawIndex >= culling.rawCommands.size())
			{
				continue;
			}
			const TombstoneIndirectCommand command = culling.rawCommands[drawIndex];
			if (command.instanceCount == 0u)
			{
				continue;
			}
			if (patch.patchedCommandCount < patch.targetCommands.size())
			{
				patch.targetCommands[patch.patchedCommandCount] = command;
			}
			++patch.patchedCommandCount;
		}
		return patch;
	}
	struct RawBootstrapFrameState
	{
		uint64_t sceneTopologyGeneration{0};
		uint32_t objectCount{0};
		bool valid{false};
	};

	class RawBootstrapHistoryModel
	{
	public:
		explicit RawBootstrapHistoryModel(uint32_t frameSlotCount)
			: m_frames(frameSlotCount)
		{
		}

		void publish(uint32_t frameIndex, uint32_t objectCount)
		{
			m_frames.at(frameIndex) = RawBootstrapFrameState{
				.sceneTopologyGeneration = m_sceneTopologyGeneration,
				.objectCount = objectCount,
				.valid = objectCount > 0u,
			};
		}

		void advanceSceneTopology()
		{
			// Deliberately retain old slot metadata: generation matching must reject
			// stale streams even before explicit slot clearing is considered.
			++m_sceneTopologyGeneration;
		}

		bool previous(uint32_t frameIndex, uint32_t& outObjectCount) const
		{
			const uint32_t previousFrameIndex =
				(frameIndex + static_cast<uint32_t>(m_frames.size()) - 1u)
				% static_cast<uint32_t>(m_frames.size());
			const RawBootstrapFrameState& state = m_frames[previousFrameIndex];
			if (!state.valid || state.objectCount == 0u
				|| state.sceneTopologyGeneration != m_sceneTopologyGeneration)
			{
				return false;
			}

			outObjectCount = state.objectCount;
			return true;
		}

	private:
		std::vector<RawBootstrapFrameState> m_frames;
		uint64_t m_sceneTopologyGeneration{1u};
	};

	enum class TemporalCountEvent : uint8_t
	{
		oldReadsToTransferBarrier,
		gpuFillCurrentSlot,
		transferToConsumersBarrier,
		depthReadPreviousSlot,
		cullingWriteCurrentSlot,
		hostWriteCurrentSlot,
	};

	struct TemporalCountSlotState
	{
		uint32_t drawCount{0u};
		uint64_t producerSubmission{0u};
		uint64_t lastConsumerSubmission{0u};
		bool produced{false};
	};

	struct TemporalCountFrameObservation
	{
		uint32_t currentSlot{0u};
		uint32_t previousSlot{0u};
		uint32_t resetSlot{0u};
		uint32_t previousBootstrapCount{0u};
		uint32_t previousSlotCountAfterCurrentReset{0u};
		uint32_t finalCurrentCount{0u};
		bool previousBootstrapValid{false};
		bool producerSignalWaitWouldRace{false};
		bool gpuResetFollowsLastConsumer{true};
		std::vector<TemporalCountEvent> events;
	};

	class TemporalCountResetRingModel
	{
	public:
		explicit TemporalCountResetRingModel(uint32_t frameSlotCount)
			: m_slots(frameSlotCount)
		{
		}

		TemporalCountFrameObservation recordFrame(uint32_t visibleDrawCount)
		{
			const uint32_t slotCount = static_cast<uint32_t>(m_slots.size());
			const uint32_t currentSlot = static_cast<uint32_t>(m_submission % slotCount);
			const uint32_t previousSlot = (currentSlot + slotCount - 1u) % slotCount;
			TemporalCountSlotState& currentState = m_slots[currentSlot];

			TemporalCountFrameObservation observation{
				.currentSlot = currentSlot,
				.previousSlot = previousSlot,
				.resetSlot = currentSlot,
				.producerSignalWaitWouldRace =
					currentState.produced
					&& currentState.lastConsumerSubmission > currentState.producerSubmission,
				.gpuResetFollowsLastConsumer =
					!currentState.produced
					|| m_submission > currentState.lastConsumerSubmission,
			};

			// The current submission orders a GPU reset after all earlier submissions.
			observation.events.push_back(TemporalCountEvent::oldReadsToTransferBarrier);
			observation.events.push_back(TemporalCountEvent::gpuFillCurrentSlot);
			currentState.drawCount = 0u;
			observation.events.push_back(TemporalCountEvent::transferToConsumersBarrier);

			if (m_submission > 0u)
			{
				TemporalCountSlotState& previousState = m_slots[previousSlot];
				observation.previousBootstrapValid = previousState.produced;
				observation.previousBootstrapCount = previousState.drawCount;
				observation.previousSlotCountAfterCurrentReset = previousState.drawCount;
				observation.events.push_back(TemporalCountEvent::depthReadPreviousSlot);
				previousState.lastConsumerSubmission = m_submission;
			}

			observation.events.push_back(TemporalCountEvent::cullingWriteCurrentSlot);
			currentState.drawCount = visibleDrawCount;
			currentState.producerSubmission = m_submission;
			currentState.lastConsumerSubmission = m_submission;
			currentState.produced = true;
			observation.finalCurrentCount = currentState.drawCount;
			++m_submission;
			return observation;
		}

	private:
		std::vector<TemporalCountSlotState> m_slots;
		uint64_t m_submission{0u};
	};

	enum class ModelTextureState : uint8_t
	{
		general,
		shaderRead,
	};

	struct PhysicalTextureStateModel
	{
		uint64_t backendImageToken{0u};
		ModelTextureState state{ModelTextureState::general};
	};

	ModelTextureState bindPhysicalTexture(PhysicalTextureStateModel& texture, uint64_t backendImageToken)
	{
		if (backendImageToken != 0u && texture.backendImageToken != backendImageToken)
		{
			texture.backendImageToken = backendImageToken;
			texture.state = ModelTextureState::general;
		}
		return texture.state;
	}

	void commitPhysicalTexture(PhysicalTextureStateModel& texture,
	                           uint64_t backendImageToken,
	                           ModelTextureState terminalState)
	{
		if (backendImageToken != 0u && texture.backendImageToken == backendImageToken)
		{
			texture.state = terminalState;
		}
	}

	struct TemporalPostStateModel
	{
		std::array<ModelTextureState, 5> persistentTextures{};
		std::array<ModelTextureState, 2> historyTextures{};
		std::array<ModelTextureState, 9> bloomTextures{};
		bool historyValid{false};
		uint32_t historyWriteParity{0u};

		TemporalPostStateModel()
		{
			resetForCreatedResources();
		}

		void resetForCreatedResources()
		{
			persistentTextures.fill(ModelTextureState::general);
			historyTextures.fill(ModelTextureState::general);
			bloomTextures.fill(ModelTextureState::general);
			historyValid = false;
			historyWriteParity = 0u;
		}

		void submitFrame(bool frameSubmitted,
		                 bool sceneRenderingSuspended,
		                 bool taaEnabled,
		                 bool taaResolveWroteHistory)
		{
			if (!frameSubmitted)
			{
				return;
			}

			// Pass dependencies are evaluated even when a pass body early-outs.
			(void)sceneRenderingSuspended;
			persistentTextures.fill(ModelTextureState::shaderRead);
			historyTextures.fill(ModelTextureState::shaderRead);
			bloomTextures.fill(ModelTextureState::shaderRead);

			if (taaEnabled && taaResolveWroteHistory)
			{
				historyValid = true;
				historyWriteParity ^= 1u;
			}
			else
			{
				historyValid = false;
			}
		}
	};

	template <size_t Size>
	bool allTexturesHaveState(const std::array<ModelTextureState, Size>& textures,
	                          ModelTextureState expected)
	{
		return std::all_of(textures.begin(), textures.end(),
		                   [expected](ModelTextureState state) { return state == expected; });
	}

	bool expect(bool condition, const char* message)
	{
		if (condition)
		{
			return true;
		}

		std::cerr << "FAILED: " << message << '\n';
		return false;
	}

	bool expectNear(float actual, float expected, const char* message)
	{
		if (std::abs(actual - expected) <= 1.0e-6f)
		{
			return true;
		}

		std::cerr << "FAILED: " << message << " (actual=" << actual
		          << ", expected=" << expected << ")\n";
		return false;
	}
}

int main()
{
	bool passed = true;

	for (uint32_t scanStepCount = 0u; scanStepCount <= 10u; ++scanStepCount)
	{
		const uint32_t paddedElementCount = 1u << scanStepCount;
		const VisibilityPatchScanModel scanModel =
			modelVisibilityPatchScan(paddedElementCount);
		passed &= expect(
			scanModel.scanStepCount == scanStepCount,
			"visibility scan model covers every expected ping-pong step");
		passed &= expect(
			scanModel.finalPrefixBufferIndex == (scanStepCount & 1u),
			"visibility scan final prefix buffer follows odd/even ping-pong parity");
		passed &= expect(
			scanModel.producerReadAfterWriteBarrierCount == scanStepCount + 1u,
			"each visibility prefix producer has a compute RAW barrier");
		passed &= expect(
			scanModel.prefixBufferRead[0],
			"prefix A is read before the next category overwrites it");
		passed &= expect(
			scanStepCount == 0u || scanModel.prefixBufferRead[1],
			"non-trivial scans read both ping-pong prefix buffers");
		passed &= expect(
			scanModel.categoryReuseWriteAfterReadBarrierCount == 1u,
			"each subsequent visibility category has one compute WAR reuse barrier");
		passed &= expect(
			scanModel.finalIndirectReadBarrierCount == 1u,
			"each visibility category retains one compute-to-indirect barrier");
	}

	const VisibilitySortRecordingModel emptySort =
		modelVisibilitySortRecording(0u);
	passed &= expect(
		emptySort.paddedElementCount == 0u
		&& emptySort.keyCopyCount == 0u
		&& emptySort.valueCopyCount == 0u
		&& emptySort.transferToComputeBarrierCount == 0u
		&& emptySort.bitonicDispatchCount == 0u,
		"zero-element visibility sort records no copy, barrier, or dispatch");

	const VisibilitySortRecordingModel singletonSort =
		modelVisibilitySortRecording(1u);
	passed &= expect(
		singletonSort.paddedElementCount == 1u
		&& singletonSort.keyCopyCount == 1u
		&& singletonSort.valueCopyCount == 1u,
		"single-element visibility sort still copies one GPU key/value pair");
	passed &= expect(
		singletonSort.transferToComputeBarrierCount == 1u
		&& singletonSort.bitonicDispatchCount == 0u,
		"single-element visibility sort keeps copy-to-patch visibility and skips only bitonic dispatch");

	const VisibilitySortRecordingModel pairSort =
		modelVisibilitySortRecording(2u);
	passed &= expect(
		pairSort.paddedElementCount == 2u
		&& pairSort.keyCopyCount == 1u
		&& pairSort.valueCopyCount == 1u
		&& pairSort.transferToComputeBarrierCount == 1u
		&& pairSort.bitonicDispatchCount == 1u,
		"two-element visibility sort copies inputs and records one bitonic step");

	const VisibilitySortRecordingModel multiSort =
		modelVisibilitySortRecording(3u);
	passed &= expect(
		multiSort.paddedElementCount == 4u
		&& multiSort.keyCopyCount == 1u
		&& multiSort.valueCopyCount == 1u
		&& multiSort.transferToComputeBarrierCount == 1u
		&& multiSort.bitonicDispatchCount == 3u,
		"multi-element visibility sort pads, copies, and records every bitonic step");

	const TombstoneCullModel deletedBeforeFirstRender = modelTombstoneCull({
		TombstoneCullInput{
			.tombstone = true,
			.visible = true,
			.category = TombstoneCullCategory::opaque,
			.indexCount = 36u,
		},
	});
	const TombstoneVisibilityPatchModel deletedBeforeFirstRenderPatch =
		modelTombstoneVisibilityPatch(deletedBeforeFirstRender, {}, 1u);
	passed &= expect(
		deletedBeforeFirstRender.totalCount == 0u
		&& deletedBeforeFirstRender.opaqueCount == 0u
		&& deletedBeforeFirstRender.transparentCount == 0u
		&& deletedBeforeFirstRender.visibleCount == 0u
		&& deletedBeforeFirstRender.totalDrawCount == 0u,
		"deleting a meshlet before first render increments no culling or draw count");
	passed &= expect(
		deletedBeforeFirstRender.rawCommands.size() == 1u
		&& isZeroCommand(deletedBeforeFirstRender.rawCommands[0]),
		"first-render tombstone overwrites an uninitialized or stale raw indirect command with zero");
	passed &= expect(
		deletedBeforeFirstRenderPatch.patchedCommandCount == 0u
		&& deletedBeforeFirstRender.totalDrawCount
			== deletedBeforeFirstRenderPatch.patchedCommandCount,
		"first-render tombstone draw count equals the zero commands actually patched");

	const TombstoneCullModel partiallyCulledLiveMeshlets = modelTombstoneCull({
		TombstoneCullInput{
			.tombstone = true,
			.visible = true,
			.category = TombstoneCullCategory::opaque,
			.indexCount = 30u,
		},
		TombstoneCullInput{
			.visible = true,
			.category = TombstoneCullCategory::opaque,
			.indexCount = 31u,
		},
		TombstoneCullInput{
			.visible = false,
			.category = TombstoneCullCategory::opaque,
			.indexCount = 32u,
		},
		TombstoneCullInput{
			.visible = true,
			.category = TombstoneCullCategory::alphaTest,
			.indexCount = 33u,
		},
		TombstoneCullInput{
			.visible = false,
			.category = TombstoneCullCategory::transparent,
			.indexCount = 34u,
		},
		TombstoneCullInput{
			.visible = true,
			.category = TombstoneCullCategory::transparent,
			.indexCount = 35u,
		},
	});
	const TombstoneVisibilityPatchModel opaquePatch = modelTombstoneVisibilityPatch(
		partiallyCulledLiveMeshlets, {1u, 2u}, 2u);
	const TombstoneVisibilityPatchModel alphaTestPatch = modelTombstoneVisibilityPatch(
		partiallyCulledLiveMeshlets, {3u}, 1u);
	const TombstoneVisibilityPatchModel transparentPatch = modelTombstoneVisibilityPatch(
		partiallyCulledLiveMeshlets, {4u, 5u}, 2u);
	passed &= expect(
		partiallyCulledLiveMeshlets.totalCount == 5u
		&& partiallyCulledLiveMeshlets.opaqueCount == 3u
		&& partiallyCulledLiveMeshlets.transparentCount == 2u
		&& partiallyCulledLiveMeshlets.visibleCount == 3u,
		"tombstones are excluded while live opaque, alpha, and transparent classifications remain exact");
	passed &= expect(
		isZeroCommand(partiallyCulledLiveMeshlets.rawCommands[0])
		&& partiallyCulledLiveMeshlets.rawCommands[2].instanceCount == 0u
		&& partiallyCulledLiveMeshlets.rawCommands[4].instanceCount == 0u,
		"tombstone and partially culled live meshlets publish non-executable raw commands");
	passed &= expect(
		partiallyCulledLiveMeshlets.opaqueDrawCount == opaquePatch.patchedCommandCount
		&& partiallyCulledLiveMeshlets.alphaTestDrawCount == alphaTestPatch.patchedCommandCount
		&& partiallyCulledLiveMeshlets.transparentDrawCount == transparentPatch.patchedCommandCount,
		"each indirect category count equals the commands actually patched");
	passed &= expect(
		partiallyCulledLiveMeshlets.totalDrawCount
			== opaquePatch.patchedCommandCount
			+ alphaTestPatch.patchedCommandCount
			+ transparentPatch.patchedCommandCount
		&& partiallyCulledLiveMeshlets.totalDrawCount == 3u,
		"total indirect count equals the complete set of actually patched live commands");
	passed &= expect(
		opaquePatch.targetCommands[0].firstInstance == 1u
		&& alphaTestPatch.targetCommands[0].firstInstance == 3u
		&& transparentPatch.targetCommands[0].firstInstance == 5u
		&& opaquePatch.targetCommands[1].firstInstance
			== staleIndirectCommand().firstInstance,
		"culled entries leave stale capacity outside the exact draw count and cannot execute");
	RawBootstrapHistoryModel rawBootstrap(3u);
	uint32_t previousObjectCount = 0u;
	rawBootstrap.publish(0u, 12u);
	passed &= expect(
		rawBootstrap.previous(1u, previousObjectCount) && previousObjectCount == 12u,
		"normal consecutive frames may consume matching-generation raw bootstrap");

	rawBootstrap.advanceSceneTopology();
	previousObjectCount = 0u;
	passed &= expect(
		!rawBootstrap.previous(1u, previousObjectCount),
		"hot-switch first frame rejects the previous scene's raw bootstrap");
	rawBootstrap.publish(1u, 7u);
	passed &= expect(
		rawBootstrap.previous(2u, previousObjectCount) && previousObjectCount == 7u,
		"raw bootstrap resumes after the hot-switched scene publishes its own stream");

	rawBootstrap.advanceSceneTopology();
	previousObjectCount = 0u;
	passed &= expect(
		!rawBootstrap.previous(2u, previousObjectCount),
		"deletion first frame rejects the pre-delete raw draw stream");
	rawBootstrap.publish(2u, 6u);
	passed &= expect(
		rawBootstrap.previous(0u, previousObjectCount) && previousObjectCount == 6u,
		"raw bootstrap resumes after deletion publishes the reduced topology");

	rawBootstrap.advanceSceneTopology();
	previousObjectCount = 0u;
	passed &= expect(
		!rawBootstrap.previous(0u, previousObjectCount),
		"reorder first frame rejects the previous draw-index ordering");
	rawBootstrap.publish(0u, 6u);
	passed &= expect(
		rawBootstrap.previous(1u, previousObjectCount) && previousObjectCount == 6u,
		"raw bootstrap resumes on the consecutive frame after reorder publication");

	rawBootstrap.advanceSceneTopology();
	previousObjectCount = 0u;
	passed &= expect(
		!rawBootstrap.previous(1u, previousObjectCount),
		"scene clear or rebuild cannot execute a stale previous raw stream");

	const std::array<uint32_t, 8> temporalDrawCounts = {
		19u, // many
		1u,  // one
		0u,  // zero after a prior non-zero slot value
		37u, // many
		0u,
		1u,
		53u,
		0u,
	};
	for (const uint32_t frameSlotCount : {2u, 3u})
	{
		TemporalCountResetRingModel countRing(frameSlotCount);
		for (uint32_t frameNumber = 0u;
		     frameNumber < static_cast<uint32_t>(temporalDrawCounts.size());
		     ++frameNumber)
		{
			const TemporalCountFrameObservation observation =
				countRing.recordFrame(temporalDrawCounts[frameNumber]);
			passed &= expect(
				observation.currentSlot == frameNumber % frameSlotCount
				&& observation.resetSlot == observation.currentSlot,
				"temporal count reset always targets the current recycled frame slot");
			passed &= expect(
				observation.events.size() >= 4u
				&& observation.events[0] == TemporalCountEvent::oldReadsToTransferBarrier
				&& observation.events[1] == TemporalCountEvent::gpuFillCurrentSlot
				&& observation.events[2] == TemporalCountEvent::transferToConsumersBarrier
				&& observation.events.back() == TemporalCountEvent::cullingWriteCurrentSlot,
				"GPU count reset records old-read barrier, fill, consumer barrier, then culling");
			passed &= expect(
				std::find(observation.events.begin(), observation.events.end(),
				          TemporalCountEvent::hostWriteCurrentSlot)
					== observation.events.end(),
				"temporal count reset never performs a host write");
			passed &= expect(
				observation.finalCurrentCount == temporalDrawCounts[frameNumber],
				"GPU fill followed by culling preserves zero, one, and many draw counts");

			if (frameNumber > 0u)
			{
				passed &= expect(
					observation.previousBootstrapValid
					&& observation.previousBootstrapCount == temporalDrawCounts[frameNumber - 1u]
					&& observation.previousSlotCountAfterCurrentReset
						== temporalDrawCounts[frameNumber - 1u],
					"resetting the current slot does not zero the previous-slot depth bootstrap");
			}

			if (frameNumber >= frameSlotCount)
			{
				passed &= expect(
					observation.producerSignalWaitWouldRace,
					"2/3-slot wraparound exposes a consumer newer than the slot producer signal");
				passed &= expect(
					observation.gpuResetFollowsLastConsumer,
					"same-queue GPU reset submission follows the last previous-slot consumer");
			}
		}
	}

	PhysicalTextureStateModel physicalTexture{};
	passed &= expect(
		bindPhysicalTexture(physicalTexture, 101u) == ModelTextureState::general,
		"a newly created physical texture starts from its real General layout");
	commitPhysicalTexture(physicalTexture, 101u, ModelTextureState::shaderRead);
	passed &= expect(
		bindPhysicalTexture(physicalTexture, 101u) == ModelTextureState::shaderRead,
		"rebinding the same physical texture preserves the previous terminal layout");
	passed &= expect(
		bindPhysicalTexture(physicalTexture, 202u) == ModelTextureState::general,
		"a recreated physical texture cannot inherit the old image's ShaderRead layout");
	commitPhysicalTexture(physicalTexture, 101u, ModelTextureState::shaderRead);
	passed &= expect(
		physicalTexture.state == ModelTextureState::general,
		"a stale token cannot publish terminal state into a replacement texture");

	TemporalPostStateModel temporalPost{};
	passed &= expect(
		allTexturesHaveState(temporalPost.persistentTextures, ModelTextureState::general)
		&& allTexturesHaveState(temporalPost.historyTextures, ModelTextureState::general)
		&& allTexturesHaveState(temporalPost.bloomTextures, ModelTextureState::general)
		&& !temporalPost.historyValid && temporalPost.historyWriteParity == 0u,
		"init starts persistent post textures and TAA parity from creation state");

	temporalPost.submitFrame(true, false, true, true);
	passed &= expect(
		allTexturesHaveState(temporalPost.persistentTextures, ModelTextureState::shaderRead)
		&& allTexturesHaveState(temporalPost.historyTextures, ModelTextureState::shaderRead)
		&& allTexturesHaveState(temporalPost.bloomTextures, ModelTextureState::shaderRead)
		&& temporalPost.historyValid && temporalPost.historyWriteParity == 1u,
		"a normal TAA frame publishes sampled terminal layouts and advances parity");

	temporalPost.submitFrame(false, true, false, false);
	passed &= expect(
		allTexturesHaveState(temporalPost.persistentTextures, ModelTextureState::shaderRead)
		&& temporalPost.historyValid && temporalPost.historyWriteParity == 1u,
		"a prepare/acquire no-op preserves physical layouts, validity, and parity");

	temporalPost.submitFrame(true, true, true, false);
	passed &= expect(
		allTexturesHaveState(temporalPost.persistentTextures, ModelTextureState::shaderRead)
		&& allTexturesHaveState(temporalPost.historyTextures, ModelTextureState::shaderRead)
		&& allTexturesHaveState(temporalPost.bloomTextures, ModelTextureState::shaderRead)
		&& !temporalPost.historyValid && temporalPost.historyWriteParity == 1u,
		"a submitted suspended frame keeps dependency terminal layouts without advancing history");

	temporalPost.submitFrame(true, false, false, false);
	passed &= expect(
		allTexturesHaveState(temporalPost.persistentTextures, ModelTextureState::shaderRead)
		&& !temporalPost.historyValid && temporalPost.historyWriteParity == 1u,
		"a TAA-off frame keeps sampled terminal layouts and leaves parity unchanged");

	temporalPost.submitFrame(true, false, true, true);
	passed &= expect(
		temporalPost.historyValid && temporalPost.historyWriteParity == 0u,
		"reenabling TAA advances parity only after the resolve writes history");

	temporalPost.resetForCreatedResources();
	passed &= expect(
		allTexturesHaveState(temporalPost.persistentTextures, ModelTextureState::general)
		&& allTexturesHaveState(temporalPost.historyTextures, ModelTextureState::general)
		&& allTexturesHaveState(temporalPost.bloomTextures, ModelTextureState::general)
		&& !temporalPost.historyValid && temporalPost.historyWriteParity == 0u,
		"shutdown then init resets every recreated texture and TAA history contract");

	passed &= expectNear(
		rebuildCurrentVisibleDepth({0.72f}),
		0.72f,
		"a newly exposed current-visible surface writes real reverse-Z depth");

	passed &= expectNear(
		rebuildCurrentVisibleDepth({0.35f, 0.81f, 0.62f}),
		0.81f,
		"current-visible depth rebuild keeps the nearest reverse-Z surface");

	passed &= expectNear(
		rebuildCurrentVisibleDepth({0.62f, 0.81f, 0.35f}),
		0.81f,
		"current-visible depth rebuild is independent of draw order");

	const float stalePreviousVisibleDepth = 0.90f;
	const float newlyExposedCurrentDepth = 0.72f;
	passed &= expectNear(
		loadBootstrapThenPatch(stalePreviousVisibleDepth, {newlyExposedCurrentDepth}),
		stalePreviousVisibleDepth,
		"loading bootstrap depth demonstrates the stale-depth rejection");
	passed &= expectNear(
		rebuildCurrentVisibleDepth({newlyExposedCurrentDepth}),
		newlyExposedCurrentDepth,
		"clearing bootstrap depth prevents stale previous visibility from occluding current visibility");

	passed &= expectNear(
		rebuildCurrentVisibleDepth({0.72f, 0.72f}),
		0.72f,
		"GREATER_OR_EQUAL accepts equal-depth opaque and alpha-tested redraws");

	DepthImage singleNearPixel = makeDepthImage(8u, 8u, 0.0f);
	singleNearPixel.at(4u, 4u) = 0.92f;
	const DepthImage singleNearMip = reduceSourceDepthToPyramidMip0(singleNearPixel);
	passed &= expect(
		!isConservativelyOccluded(singleNearMip, 0u, 0u, 3u, 3u, 0.45f),
		"a single near pixel cannot occlude an object's complete footprint");

	DepthImage contour = makeDepthImage(8u, 8u, 0.86f);
	for (uint32_t y = 0; y < contour.height; ++y)
	{
		contour.at(7u, y) = 0.0f;
	}
	const DepthImage contourMip = reduceSourceDepthToPyramidMip0(contour);
	passed &= expect(
		!isConservativelyOccluded(contourMip, 0u, 0u, 3u, 3u, 0.45f),
		"an uncovered silhouette edge keeps the object visible");

	DepthImage alphaHole = makeDepthImage(8u, 8u, 0.88f);
	alphaHole.at(3u, 3u) = alphaMaskWritesDepth(1.0f, true, 0.0f, 0.5f) ? 0.88f : 0.0f;
	const DepthImage alphaHoleMip = reduceSourceDepthToPyramidMip0(alphaHole);
	passed &= expect(
		!isConservativelyOccluded(alphaHoleMip, 0u, 0u, 3u, 3u, 0.45f),
		"an alpha-mask hole contributes far depth and prevents false occlusion");

	passed &= expect(
		!alphaMaskWritesDepth(0.25f, false, 1.0f, 0.5f),
		"a textureless MASK material below baseColorFactor alpha cutoff discards");
	passed &= expect(
		alphaMaskWritesDepth(0.75f, false, 0.0f, 0.5f),
		"a textureless MASK material above baseColorFactor alpha cutoff writes depth");

	const DepthImage fullCoverageMip =
		reduceSourceDepthToPyramidMip0(makeDepthImage(8u, 8u, 0.84f));
	passed &= expect(
		isConservativelyOccluded(fullCoverageMip, 0u, 0u, 3u, 3u, 0.45f),
		"a fully covered footprint is culled by the reverse-Z farthest-depth bound");

	const DepthImage oddEdgeMip =
		reduceSourceDepthToPyramidMip0(makeDepthImage(7u, 5u, 0.90f));
	passed &= expect(
		!isConservativelyOccluded(
			oddEdgeMip,
			oddEdgeMip.width - 1u,
			oddEdgeMip.height - 1u,
			oddEdgeMip.width - 1u,
			oddEdgeMip.height - 1u,
			0.45f),
		"an incomplete screen-edge reduction block stays conservatively visible");

	const DepthImage oddSourceMip0 =
		reduceSourceDepthToPyramidMip0(makeDepthImage(9u, 5u, 0.90f));
	passed &= expect(
		oddSourceMip0.width == 5u && oddSourceMip0.height == 3u,
		"source-to-mip0 keeps the conservative ceil extent for partial 2x2 blocks");

	const DepthImage oddVulkanMip1 = reduceVulkanPyramidMipMin2x2(oddSourceMip0);
	passed &= expect(
		oddVulkanMip1.width == 2u && oddVulkanMip1.height == 1u,
		"Vulkan image mip dimensions floor 5x3 to 2x1 instead of ceil 3x2");
	passed &= expect(
		oddVulkanMip1.width != (oddSourceMip0.width + 1u) / 2u,
		"the odd-width counterexample distinguishes Vulkan floor mips from a ceil model");
	passed &= expect(
		isFootprintDimensionRepresented(0.0f, 8.0f, 9u, 5u, 1u)
		&& !isFootprintDimensionRepresented(8.0f, 9.0f, 9u, 5u, 1u),
		"a floor-sized mip represents the first eight source pixels but not the omitted edge strip");

	const ModelMatrix4 orthographicProjection =
		makeReverseZOrthographic(-20.0f, 30.0f, -12.0f, 18.0f, 0.1f, 100.0f);
	const ModelFloat3 orthographicNearCenter{.x = 7.0f, .y = -3.0f, .z = -20.0f};
	const ModelFloat3 orthographicFarCenter{.x = 7.0f, .y = -3.0f, .z = -60.0f};
	const ProjectedSphereBounds orthographicNearBounds =
		projectConservativeSphereAabb(
			orthographicProjection, orthographicNearCenter, 4.0f);
	const ProjectedSphereBounds orthographicFarBounds =
		projectConservativeSphereAabb(
			orthographicProjection, orthographicFarCenter, 4.0f);
	passed &= expect(
		orthographicNearBounds.valid && orthographicFarBounds.valid
		&& projectedSphereSamplesFit(
			orthographicProjection, orthographicNearCenter, 4.0f, orthographicNearBounds)
		&& projectedSphereSamplesFit(
			orthographicProjection, orthographicFarCenter, 4.0f, orthographicFarBounds),
		"eight projected AABB corners conservatively contain orthographic sphere footprints");
	passed &= expectNear(
		orthographicNearBounds.minX,
		orthographicFarBounds.minX,
		"orthographic footprint width does not shrink with view-space depth");
	passed &= expectNear(
		orthographicNearBounds.maxY,
		orthographicFarBounds.maxY,
		"orthographic footprint height does not use perspective radius-over-z scaling");

	const ModelMatrix4 offAxisWideProjection =
		makeReverseZPerspective(0.30f, -0.35f, -0.40f, 0.10f, 0.1f, 100.0f);
	const ModelFloat3 offAxisCenter{.x = 6.0f, .y = 0.0f, .z = -8.0f};
	const ProjectedSphereBounds offAxisBounds =
		projectConservativeSphereAabb(offAxisWideProjection, offAxisCenter, 2.0f);
	passed &= expect(
		offAxisBounds.valid
		&& projectedSphereSamplesFit(
			offAxisWideProjection, offAxisCenter, 2.0f, offAxisBounds),
		"eight projected AABB corners conservatively contain off-axis wide-FOV footprints");
	passed &= expect(
		perspectiveSphereExceedsNaiveCenterRadiusBound(
			offAxisWideProjection, offAxisCenter, 2.0f),
		"an off-center perspective sphere is a counterexample to center plus scale-radius-over-z");

	const ModelMatrix4 nearClipProjection =
		makeReverseZPerspective(1.0f, -1.0f, 0.0f, 0.0f, 0.5f, 100.0f);
	passed &= expect(
		!projectConservativeSphereAabb(
			nearClipProjection,
			ModelFloat3{.x = 0.0f, .y = 0.0f, .z = -0.7f},
			0.3f).valid,
		"a sphere whose AABB crosses the near plane stays visible");
	passed &= expect(
		!projectConservativeSphereAabb(
			offAxisWideProjection,
			ModelFloat3{.x = 0.0f, .y = 0.0f, .z = -3.0f},
			4.0f).valid,
		"a huge sphere crossing the camera plane stays visible");

	const float rotationComponent = std::sqrt(0.5f);
	const ModelMatrix3 parentNonUniformTimesChildRotation{
		.rows = {{
			ModelFloat3{.x = 2.0f * rotationComponent, .y = -2.0f * rotationComponent},
			ModelFloat3{.x = rotationComponent, .y = rotationComponent},
			ModelFloat3{.z = 1.0f},
		}},
	};
	const ModelFloat3 maximumStretchDirection{
		.x = rotationComponent,
		.y = -rotationComponent,
	};
	const float actualStretch =
		length3(multiply(parentNonUniformTimesChildRotation, maximumStretchDirection));
	const float legacyRadiusScale =
		legacyMaxColumnRadiusScale(parentNonUniformTimesChildRotation);
	const float safeFrobeniusScale =
		frobeniusRadiusScale(parentNonUniformTimesChildRotation);
	passed &= expect(
		legacyRadiusScale + 1.0e-4f < actualStretch,
		"max column length underestimates parent non-uniform scale times child rotation");
	passed &= expect(
		safeFrobeniusScale + 1.0e-4f >= actualStretch,
		"Frobenius meshlet radius covers the parent-scale child-rotation counterexample");

	const ModelFloat3 localConeAxis{.x = 1.0f};
	ModelFloat3 transformedConeAxis{};

	const ModelMatrix3 mirroredX{
		.rows = {{
			ModelFloat3{.x = -1.0f},
			ModelFloat3{.y = 1.0f},
			ModelFloat3{.z = 1.0f},
		}},
	};
	passed &= expect(
		!tryTransformConeAxisForCulling(mirroredX, localConeAxis, transformedConeAxis),
		"negative determinant diag(-1,1,1) conservatively disables meshlet cone culling");

	const ModelMatrix3 negativeNonUniformScaleTimesRotation{
		.rows = {{
			ModelFloat3{
				.x = -2.0f * rotationComponent,
				.y = 2.0f * rotationComponent,
			},
			ModelFloat3{
				.x = rotationComponent,
				.y = rotationComponent,
			},
			ModelFloat3{.z = 1.0f},
		}},
	};
	const float negativeNonUniformDeterminant = dot3(
		negativeNonUniformScaleTimesRotation.rows[0],
		cross3(
			negativeNonUniformScaleTimesRotation.rows[1],
			negativeNonUniformScaleTimesRotation.rows[2]));
	passed &= expect(
		negativeNonUniformDeterminant < 0.0f
		&& !tryTransformConeAxisForCulling(
			negativeNonUniformScaleTimesRotation, localConeAxis, transformedConeAxis),
		"negative non-uniform scale times rotation conservatively disables meshlet cone culling");

	const ModelMatrix3 uniformScaleRotation{
		.rows = {{
			ModelFloat3{.x = 2.0f * rotationComponent, .y = -2.0f * rotationComponent},
			ModelFloat3{.x = 2.0f * rotationComponent, .y = 2.0f * rotationComponent},
			ModelFloat3{.z = 2.0f},
		}},
	};
	const float positiveDeterminant = dot3(
		uniformScaleRotation.rows[0],
		cross3(uniformScaleRotation.rows[1], uniformScaleRotation.rows[2]));
	passed &= expect(
		positiveDeterminant > 0.0f
		&& tryTransformConeAxisForCulling(
			uniformScaleRotation, localConeAxis, transformedConeAxis)
		&& std::abs(transformedConeAxis.x - rotationComponent) <= 1.0e-6f
		&& std::abs(transformedConeAxis.y - rotationComponent) <= 1.0e-6f
		&& std::abs(transformedConeAxis.z) <= 1.0e-6f,
		"positive determinant similarity transform keeps conservative cone culling enabled");

	const ModelMatrix3 parentNonUniformTimesChildRotationShear{
		.rows = {{
			ModelFloat3{.x = 2.0f * rotationComponent, .y = 2.0f * rotationComponent},
			ModelFloat3{.x = rotationComponent, .y = 3.0f * rotationComponent},
			ModelFloat3{.z = 1.0f},
		}},
	};
	const ModelFloat3 legacyLinearConeAxis =
		normalize3(multiply(parentNonUniformTimesChildRotationShear, localConeAxis));
	ModelFloat3 inverseTransposeConeAxis{};
	const bool inverseTransposeValid = tryInverseTransposeNormalAxis(
		parentNonUniformTimesChildRotationShear,
		localConeAxis,
		inverseTransposeConeAxis);
	const float coneCutoff = 0.90f;
	passed &= expect(
		inverseTransposeValid
		&& dot3(legacyLinearConeAxis, legacyLinearConeAxis) >= coneCutoff
		&& dot3(legacyLinearConeAxis, inverseTransposeConeAxis) < coneCutoff,
		"direct linear normal-cone transform falsely culls a parent non-uniform scale "
		"times child rotation/shear counterexample");
	passed &= expect(
		!tryTransformConeAxisForCulling(
			parentNonUniformTimesChildRotationShear, localConeAxis, transformedConeAxis),
		"non-similarity detection disables cone culling for parent scale plus rotation/shear");

	const ModelMatrix3 singularTransform{
		.rows = {{
			ModelFloat3{.x = 1.0f},
			ModelFloat3{},
			ModelFloat3{.z = 1.0f},
		}},
	};
	const ModelMatrix3 nearZeroTransform{
		.rows = {{
			ModelFloat3{.x = 1.0e-6f},
			ModelFloat3{.y = 1.0e-6f},
			ModelFloat3{.z = 1.0e-6f},
		}},
	};
	ModelMatrix3 nonFiniteTransform = uniformScaleRotation;
	nonFiniteTransform.rows[0].x = std::numeric_limits<float>::infinity();
	passed &= expect(
		!tryTransformConeAxisForCulling(
			singularTransform, localConeAxis, transformedConeAxis)
		&& !tryTransformConeAxisForCulling(
			nearZeroTransform, localConeAxis, transformedConeAxis)
		&& !tryTransformConeAxisForCulling(
			nonFiniteTransform, localConeAxis, transformedConeAxis),
		"near-zero, singular, and non-finite transforms conservatively disable meshlet cone culling");

	return passed ? 0 : 1;
}
