#include "../render/GPUSceneRegistry.h"
#include "../render/GPUMeshletBuffer.h"
#include "../rhi/RHICommandBuffer.h"
#include "../rhi/RHIDevice.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace demo;

	struct CopyRecord
	{
		rhi::BufferHandle source{};
		uint64_t sourceOffset{0};
		rhi::BufferHandle destination{};
		uint64_t destinationOffset{0};
		uint64_t size{0};
	};

	struct BarrierRecord
	{
		rhi::StageFlags producer{rhi::StageFlags::none};
		rhi::StageFlags consumer{rhi::StageFlags::none};
		rhi::HazardFlags hazards{rhi::HazardFlags::none};
	};

	class FakeDevice final : public rhi::Device
	{
	public:
		struct BufferStorage
		{
			rhi::BufferDesc desc{};
			std::string debugName;
			std::vector<std::byte> bytes;
			uint32_t mapCount{0};
			uint32_t unmapCount{0};
			bool alive{true};
		};

		void init(const rhi::DeviceCreateInfo&) override {}
		void deinit() override {}
		rhi::BackendInfo getBackendInfo() const override { return {.type = rhi::BackendType::vulkan, .apiName = "fake"}; }
		const char* getDeviceName() const override { return "GPUSceneRegistryFakeDevice"; }
		const rhi::PhysicalDeviceInfo& getPhysicalDeviceInfo() const override { return m_physicalDeviceInfo; }
		const rhi::DeviceFeatureInfo& getEnabledFeatureInfo() const override { return m_featureInfo; }
		rhi::CapabilityReport queryCapabilities() const override { return {}; }
		bool supports(rhi::CapabilityTier) const override { return false; }
		const rhi::MemoryProperties& getPhysicalMemoryProperties() const override { return m_memoryProperties; }
		void waitIdle() override {}
		rhi::Queue* getQueue(rhi::QueueClass) override { return nullptr; }
		std::unique_ptr<rhi::CommandAllocator> createCommandAllocator(rhi::QueueClass) override { return nullptr; }
		void initSurface(rhi::Surface&, const rhi::WindowHandle&) override {}
		std::unique_ptr<rhi::Swapchain> createSwapchain(rhi::Surface&, bool) override { return nullptr; }

		rhi::TextureViewHandle createTextureView(const rhi::TextureViewCreateDesc&) override { return {}; }
		void destroyTextureView(rhi::TextureViewHandle) override {}
		rhi::TextureHandle createTexture(const rhi::TextureDesc&) override { return {}; }
		void destroyTexture(rhi::TextureHandle) override {}

		rhi::SamplerHandle createSampler(const rhi::SamplerDesc&) override { return {}; }
		void destroySampler(rhi::SamplerHandle) override {}
		rhi::ArgumentLayoutHandle createArgumentLayout(const rhi::ArgumentLayoutDesc&) override { return {}; }
		void destroyArgumentLayout(rhi::ArgumentLayoutHandle) override {}
		rhi::ArgumentTableHandle createArgumentTable(const rhi::ArgumentTableCreateDesc&) override { return {}; }
		void destroyArgumentTable(rhi::ArgumentTableHandle) override {}
		void updateArgumentTable(rhi::ArgumentTableHandle, rhi::ArgumentWriteBatch) override {}
		rhi::ArgumentLayoutHandle getArgumentTableLayout(rhi::ArgumentTableHandle) const override { return {}; }
		rhi::PipelineHandle createGraphicsPipeline(const rhi::GraphicsPipelineDesc&) override { return {}; }
		rhi::PipelineHandle createComputePipeline(const rhi::ComputePipelineDesc&) override { return {}; }
		void destroyPipeline(rhi::PipelineHandle) override {}
		rhi::QueryPoolHandle createQueryPool(uint32_t) override { return {}; }
		void destroyQueryPool(rhi::QueryPoolHandle) override {}
		uint64_t getQueryPoolResult(rhi::QueryPoolHandle, uint32_t) override { return 0; }
		bool getQueryPoolResultsWithAvailability(rhi::QueryPoolHandle, uint32_t, std::span<uint64_t>) override { return false; }
		rhi::ShaderLibraryHandle createShaderLibrary(const rhi::ShaderLibraryDesc&) override { return {}; }
		void destroyShaderLibrary(rhi::ShaderLibraryHandle) override {}

		rhi::BufferHandle createBuffer(const rhi::BufferDesc& desc) override
		{
			if (desc.size == 0u || desc.size > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
			{
				throw std::runtime_error("invalid fake buffer size");
			}

			const rhi::BufferHandle handle{
				.index = static_cast<uint32_t>(m_buffers.size()),
				.generation = 1u,
			};
			BufferStorage storage{};
			storage.desc = desc;
			storage.debugName = desc.debugName != nullptr ? desc.debugName : "";
			storage.bytes.resize(static_cast<size_t>(desc.size));
			m_buffers.push_back(std::move(storage));
			return handle;
		}

		void destroyBuffer(rhi::BufferHandle handle) override
		{
			buffer(handle).alive = false;
		}

		rhi::GpuPtr getBufferGpuAddress(rhi::BufferHandle handle) const override
		{
			(void)buffer(handle);
			return rhi::GpuPtr{0x100000ull + static_cast<uint64_t>(handle.index) * 0x1000ull};
		}

		void* mapBuffer(rhi::BufferHandle handle) override
		{
			BufferStorage& storage = buffer(handle);
			++storage.mapCount;
			return storage.bytes.data();
		}

		void unmapBuffer(rhi::BufferHandle handle) override
		{
			++buffer(handle).unmapCount;
		}

		void executeCopy(const CopyRecord& copy)
		{
			const BufferStorage& source = buffer(copy.source);
			BufferStorage& destination = buffer(copy.destination);
			if (copy.sourceOffset > source.bytes.size()
				|| copy.size > source.bytes.size() - copy.sourceOffset
				|| copy.destinationOffset > destination.bytes.size()
				|| copy.size > destination.bytes.size() - copy.destinationOffset)
			{
				throw std::runtime_error("fake copy range exceeds buffer bounds");
			}
			std::memmove(destination.bytes.data() + copy.destinationOffset,
			             source.bytes.data() + copy.sourceOffset,
			             static_cast<size_t>(copy.size));
		}

		const BufferStorage& buffer(rhi::BufferHandle handle) const
		{
			if (handle.generation != 1u || handle.index == 0u || handle.index >= m_buffers.size())
			{
				throw std::runtime_error("invalid fake buffer handle");
			}
			return m_buffers[handle.index];
		}

		BufferStorage& buffer(rhi::BufferHandle handle)
		{
			return const_cast<BufferStorage&>(static_cast<const FakeDevice&>(*this).buffer(handle));
		}

		rhi::BufferHandle latestBufferNamed(const char* debugName) const
		{
			for (size_t index = m_buffers.size(); index > 1u; --index)
			{
				const BufferStorage& storage = m_buffers[index - 1u];
				if (storage.alive && storage.debugName == debugName)
				{
					return rhi::BufferHandle{
						.index = static_cast<uint32_t>(index - 1u),
						.generation = 1u,
					};
				}
			}
			return {};
		}

	private:
		std::vector<BufferStorage> m_buffers{1};
		rhi::PhysicalDeviceInfo m_physicalDeviceInfo{};
		rhi::DeviceFeatureInfo m_featureInfo{};
		rhi::MemoryProperties m_memoryProperties{};
	};

	class FakeCommandBuffer;

	class FakeComputeEncoder final : public rhi::ComputeEncoder
	{
	public:
		explicit FakeComputeEncoder(FakeCommandBuffer* owner)
			: m_owner(owner)
		{
		}

		void setPipeline(rhi::PipelineHandle) override {}
		void setArgumentTable(uint32_t, rhi::ArgumentTableHandle) override {}
		void setRootConstants(uint32_t, std::span<const std::byte>) override {}
		void setRootPointer(uint32_t, rhi::GpuPtr) override {}
		void dispatch(const rhi::DispatchDesc&) override {}
		void dispatchIndirect(const rhi::DispatchIndirectDesc&) override {}
		void copyBuffer(rhi::BufferHandle source,
		                uint64_t sourceOffset,
		                rhi::BufferHandle destination,
		                uint64_t destinationOffset,
		                uint64_t size) override;
		void copyBufferToTexture(const rhi::BufferTextureCopyDesc&) override {}
		void copyTextureToBuffer(const rhi::BufferTextureCopyDesc&) override {}
		void blitTexture(const rhi::TextureBlitDesc&) override {}
		void fillBuffer(rhi::BufferHandle, uint64_t, uint64_t, uint32_t) override {}

	private:
		FakeCommandBuffer* m_owner{nullptr};
	};

	class FakeCommandBuffer final : public rhi::CommandBuffer
	{
	public:
		explicit FakeCommandBuffer(FakeDevice* device)
			: m_device(device), m_compute(this)
		{
		}

		void begin(rhi::CommandAllocator&) override { m_state = rhi::CommandBufferState::recording; }
		void end() override { m_state = rhi::CommandBufferState::executable; }
		[[nodiscard]] rhi::CommandBufferState state() const noexcept override { return m_state; }

		rhi::RenderEncoder* beginRenderPass(const rhi::RenderPassDesc&) override { return nullptr; }
		rhi::ComputeEncoder* beginComputePass() override { return &m_compute; }
		void endEncoding() override {}
		void barrier(rhi::StageFlags producer,
		             rhi::StageFlags consumer,
		             rhi::HazardFlags hazards) override
		{
			barriers.push_back(BarrierRecord{producer, consumer, hazards});
		}
		void resourceBarrier(std::span<const rhi::TextureBarrier>,
		                     std::span<const rhi::BufferBarrier>,
		                     std::span<const rhi::AliasingBarrier>) override {}
		void clearColorTexture(rhi::TextureHandle,
		                       const rhi::TextureSubresourceRange&,
		                       const rhi::ClearColorValue&) override {}
		void beginEvent(const char*) override {}
		void endEvent() override {}
		void resetQueryPool(rhi::QueryPoolHandle, uint32_t, uint32_t) override {}
		void writeTimestamp(rhi::QueryPoolHandle, uint32_t, bool) override {}

		void recordCopy(const CopyRecord& copy) { copies.push_back(copy); }

		void executeCopies()
		{
			for (const CopyRecord& copy : copies)
			{
				m_device->executeCopy(copy);
			}
		}

		void reset()
		{
			copies.clear();
			barriers.clear();
		}

		std::vector<CopyRecord> copies;
		std::vector<BarrierRecord> barriers;

	private:
		FakeDevice* m_device{nullptr};
		FakeComputeEncoder m_compute;
		rhi::CommandBufferState m_state{rhi::CommandBufferState::idle};
	};

	void FakeComputeEncoder::copyBuffer(rhi::BufferHandle source,
	                                    uint64_t sourceOffset,
	                                    rhi::BufferHandle destination,
	                                    uint64_t destinationOffset,
	                                    uint64_t size)
	{
		m_owner->recordCopy(CopyRecord{
			.source = source,
			.sourceOffset = sourceOffset,
			.destination = destination,
			.destinationOffset = destinationOffset,
			.size = size,
		});
	}

	void expect(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	bool hasStage(rhi::StageFlags stages, rhi::StageFlags expected)
	{
		return static_cast<uint64_t>(stages & expected) == static_cast<uint64_t>(expected);
	}

	bool sourceSlicesDoNotOverlap(const std::vector<CopyRecord>& copies)
	{
		for (size_t lhsIndex = 0; lhsIndex < copies.size(); ++lhsIndex)
		{
			const CopyRecord& lhs = copies[lhsIndex];
			if ((lhs.sourceOffset & 3u) != 0u || (lhs.size & 3u) != 0u)
			{
				return false;
			}
			for (size_t rhsIndex = lhsIndex + 1u; rhsIndex < copies.size(); ++rhsIndex)
			{
				const CopyRecord& rhs = copies[rhsIndex];
				const bool overlaps = lhs.sourceOffset < rhs.sourceOffset + rhs.size
					&& rhs.sourceOffset < lhs.sourceOffset + lhs.size;
				if (overlaps)
				{
					return false;
				}
			}
		}
		return true;
	}

	GPUSceneRegistrationDesc makeDesc(uint32_t index)
	{
		GPUSceneRegistrationDesc desc{};
		desc.meshIndex = 100u + index;
		desc.materialIndex = 200u + index;
		desc.transform = glm::mat4(1.0f);
		desc.transform[0][0] = 1.0f + static_cast<float>(index) * 0.1f;
		desc.transform[1][1] = 2.0f + static_cast<float>(index) * 0.1f;
		desc.transform[2][2] = 3.0f + static_cast<float>(index) * 0.1f;
		desc.transform[3] = glm::vec4(
			10.0f + static_cast<float>(index),
			20.0f + static_cast<float>(index),
			30.0f + static_cast<float>(index),
			1.0f);
		desc.boundsSphere = glm::vec4(
			40.0f + static_cast<float>(index),
			50.0f + static_cast<float>(index),
			60.0f + static_cast<float>(index),
			1.0f + static_cast<float>(index));
		desc.flags = 300u + index;
		desc.indexCount = 400u + index;
		desc.firstIndex = 500u + index;
		desc.vertexOffset = 600 + static_cast<int32_t>(index);
		return desc;
	}

	shaderio::GPUSceneObject expectedSceneObject(const GPUSceneRegistrationDesc& desc)
	{
		shaderio::GPUSceneObject object{};
		for (int row = 0; row < 3; ++row)
		{
			object.worldMatrixRows[row] = glm::vec4(
				desc.transform[0][row],
				desc.transform[1][row],
				desc.transform[2][row],
				desc.transform[3][row]);
		}
		object.boundsSphere = desc.boundsSphere;
		object.materialIndex = desc.materialIndex;
		object.meshIndex = desc.meshIndex;
		object.flags = desc.flags;
		return object;
	}

	shaderio::GPUCullObject expectedCullObject(const GPUSceneRegistrationDesc& desc)
	{
		return shaderio::GPUCullObject{
			.sphereCenterRadius = desc.boundsSphere,
			.indexCount = desc.indexCount,
			.firstIndex = desc.firstIndex,
			.vertexOffset = desc.vertexOffset,
			.flags = desc.flags,
		};
	}

	template <typename T>
	T readElement(const FakeDevice& device, rhi::BufferHandle bufferHandle, uint32_t index)
	{
		const FakeDevice::BufferStorage& storage = device.buffer(bufferHandle);
		const uint64_t offset = sizeof(T) * static_cast<uint64_t>(index);
		if (offset > storage.bytes.size() || sizeof(T) > storage.bytes.size() - offset)
		{
			throw std::runtime_error("fake read exceeds buffer bounds");
		}
		T result{};
		std::memcpy(&result, storage.bytes.data() + offset, sizeof(T));
		return result;
	}

	void expectRegistryContents(const FakeDevice& device,
	                            const GPUSceneRegistry& registry,
	                            const std::vector<GPUSceneRegistrationDesc>& descriptions)
	{
		for (uint32_t index = 0; index < descriptions.size(); ++index)
		{
			const shaderio::GPUSceneObject actualScene =
				readElement<shaderio::GPUSceneObject>(device, registry.getBufferHandle(), index);
			const shaderio::GPUSceneObject expectedScene = expectedSceneObject(descriptions[index]);
			expect(std::memcmp(&actualScene, &expectedScene, sizeof(actualScene)) == 0,
			       "scene-object upload bytes do not match the packed registry model");

			const shaderio::GPUCullObject actualCull =
				readElement<shaderio::GPUCullObject>(device, registry.getCullBufferHandle(), index);
			const shaderio::GPUCullObject expectedCull = expectedCullObject(descriptions[index]);
			expect(std::memcmp(&actualCull, &expectedCull, sizeof(actualCull)) == 0,
			       "cull-object upload bytes do not match the packed registry model");
		}
	}

	void verifyPostCopyBarrier(const BarrierRecord& barrier)
	{
		expect(barrier.producer == rhi::StageFlags::transfer,
		       "copy publication must originate at the transfer stage");
		expect(hasStage(barrier.consumer, rhi::StageFlags::compute)
		       && hasStage(barrier.consumer, rhi::StageFlags::vertexShader),
		       "copy publication must cover compute and vertex shader readers");
		expect(barrier.hazards == rhi::HazardFlags::bufferWrites,
		       "copy publication must expose transfer buffer writes");
	}

	void runRegistryUploadModel()
	{
		FakeDevice device;
		GPUSceneRegistry registry;
		registry.init(&device);
		FakeCommandBuffer commandBuffer(&device);

		std::vector<GPUSceneRegistrationDesc> descriptions;
		std::vector<GPUSceneObjectHandle> objectIds;
		for (uint32_t index = 0; index < 6u; ++index)
		{
			descriptions.push_back(makeDesc(index));
			objectIds.push_back(registry.registerObject(descriptions.back()));
		}

		registry.syncToGpu(commandBuffer);
		expect(commandBuffer.copies.size() == 2u,
		       "full upload must record one scene copy and one cull copy");
		expect(sourceSlicesDoNotOverlap(commandBuffer.copies),
		       "full scene and cull copies must retain disjoint staging source slices");
		expect(commandBuffer.barriers.size() == 1u,
		       "first initialization must skip the prior-reader WAR barrier");
		verifyPostCopyBarrier(commandBuffer.barriers.front());
		commandBuffer.executeCopies();
		expectRegistryContents(device, registry, descriptions);

		const rhi::BufferHandle initialStaging = device.latestBufferNamed("GPUSceneRegistry.staging");
		expect(!initialStaging.isNull(), "initial staging buffer was not allocated");
		expect(device.buffer(initialStaging).mapCount == 1u,
		       "initial staging buffer must be mapped exactly once");

		commandBuffer.reset();
		const uint32_t firstDescendant = 1u;
		const uint32_t secondDescendant = 4u;
		descriptions[firstDescendant].transform[3] = glm::vec4(101.0f, 102.0f, 103.0f, 1.0f);
		descriptions[firstDescendant].boundsSphere = glm::vec4(111.0f, 112.0f, 113.0f, 11.0f);
		descriptions[secondDescendant].transform[3] = glm::vec4(201.0f, 202.0f, 203.0f, 1.0f);
		descriptions[secondDescendant].boundsSphere = glm::vec4(211.0f, 212.0f, 213.0f, 21.0f);
		registry.updateTransform(
			objectIds[firstDescendant],
			descriptions[firstDescendant].transform,
			descriptions[firstDescendant].boundsSphere);
		registry.updateTransform(
			objectIds[secondDescendant],
			descriptions[secondDescendant].transform,
			descriptions[secondDescendant].boundsSphere);

		registry.syncToGpu(commandBuffer);
		expect(commandBuffer.copies.size() == 4u,
		       "two non-contiguous descendant ranges must record four scene/cull copies");
		expect(sourceSlicesDoNotOverlap(commandBuffer.copies),
		       "batched dirty-range copies must keep every recorded source slice disjoint");
		expect(commandBuffer.copies[0].destinationOffset
		       == sizeof(shaderio::GPUSceneObject) * firstDescendant,
		       "first scene dirty range destination offset mismatch");
		expect(commandBuffer.copies[1].destinationOffset
		       == sizeof(shaderio::GPUSceneObject) * secondDescendant,
		       "second scene dirty range destination offset mismatch");
		expect(commandBuffer.copies[2].destinationOffset
		       == sizeof(shaderio::GPUCullObject) * firstDescendant,
		       "first cull dirty range destination offset mismatch");
		expect(commandBuffer.copies[3].destinationOffset
		       == sizeof(shaderio::GPUCullObject) * secondDescendant,
		       "second cull dirty range destination offset mismatch");
		expect(commandBuffer.barriers.size() == 2u,
		       "updates of initialized registry buffers require pre-copy WAR and post-copy RAW barriers");
		expect(hasStage(commandBuffer.barriers[0].producer, rhi::StageFlags::compute)
		       && hasStage(commandBuffer.barriers[0].producer, rhi::StageFlags::vertexShader),
		       "pre-copy WAR barrier must cover all prior shader reader stages");
		expect(commandBuffer.barriers[0].consumer == rhi::StageFlags::transfer,
		       "pre-copy WAR barrier must target the transfer writer");
		expect(commandBuffer.barriers[0].hazards == rhi::HazardFlags::readBeforeWrite,
		       "pre-copy barrier must express read-before-write reuse");
		verifyPostCopyBarrier(commandBuffer.barriers[1]);
		commandBuffer.executeCopies();
		expectRegistryContents(device, registry, descriptions);

		commandBuffer.reset();
		while (descriptions.size() < 65u)
		{
			const uint32_t index = static_cast<uint32_t>(descriptions.size());
			descriptions.push_back(makeDesc(index));
			objectIds.push_back(registry.registerObject(descriptions.back()));
		}
		const rhi::BufferHandle previousObjectBuffer = registry.getBufferHandle();
		registry.syncToGpu(commandBuffer);
		const rhi::BufferHandle expandedStaging = device.latestBufferNamed("GPUSceneRegistry.staging");
		expect(registry.getBufferHandle() != previousObjectBuffer,
		       "registry object buffer must grow when object count exceeds the initial capacity");
		expect(expandedStaging != initialStaging,
		       "staging buffer must grow when the packed full upload exceeds its mapped capacity");
		expect(device.buffer(initialStaging).unmapCount == 1u
		       && !device.buffer(initialStaging).alive,
		       "staging growth must unmap and retire the previous allocation");
		expect(device.buffer(expandedStaging).mapCount == 1u,
		       "expanded staging allocation must be mapped before use");
		expect(commandBuffer.copies.size() == 2u
		       && sourceSlicesDoNotOverlap(commandBuffer.copies),
		       "capacity growth full upload must preserve disjoint object/cull slices");
		expect(commandBuffer.barriers.size() == 1u,
		       "new destination buffers must be treated as first initialization");
		verifyPostCopyBarrier(commandBuffer.barriers.front());
		commandBuffer.executeCopies();
		expectRegistryContents(device, registry, descriptions);

		registry.deinit();
		expect(device.buffer(expandedStaging).unmapCount == 1u
		       && !device.buffer(expandedStaging).alive,
		       "registry shutdown must unmap and retire the live staging allocation");
	}
	void runSharedMeshInstanceIdentityModel()
	{
		FakeDevice device;
		GPUSceneRegistry registry;
		registry.init(&device);
		FakeCommandBuffer commandBuffer(&device);

		std::vector<GPUSceneRegistrationDesc> descriptions{makeDesc(0u), makeDesc(1u)};
		const MeshHandle sharedMesh{.index = 19u, .generation = 4u};
		descriptions[0].meshHandle = sharedMesh;
		descriptions[1].meshHandle = sharedMesh;
		const GPUSceneObjectHandle firstObjectId = registry.registerObject(descriptions[0]);
		const GPUSceneObjectHandle secondObjectId = registry.registerObject(descriptions[1]);
		registry.syncToGpu(commandBuffer);
		commandBuffer.executeCopies();
		expectRegistryContents(device, registry, descriptions);

		commandBuffer.reset();
		descriptions[0].transform =
			glm::translate(glm::mat4(1.0f), glm::vec3(7.0f, -3.0f, 5.0f))
			* glm::rotate(glm::mat4(1.0f), glm::radians(31.0f), glm::normalize(glm::vec3(0.25f, 1.0f, -0.4f)))
			* glm::scale(glm::mat4(1.0f), glm::vec3(2.5f, 0.75f, 1.6f));
		descriptions[0].boundsSphere = glm::vec4(8.0f, -1.0f, 3.0f, 6.0f);
		registry.updateTransform(firstObjectId, descriptions[0].transform, descriptions[0].boundsSphere);
		registry.syncToGpu(commandBuffer);
		expect(commandBuffer.copies.size() == 2u,
		       "one shared-mesh instance update must publish exactly one scene/cull dirty range");
		commandBuffer.executeCopies();
		expectRegistryContents(device, registry, descriptions);

		const shaderio::GPUSceneObject untouchedSecond =
			readElement<shaderio::GPUSceneObject>(device, registry.getBufferHandle(), 1u);
		const shaderio::GPUSceneObject expectedSecond = expectedSceneObject(descriptions[1]);
		expect(std::memcmp(&untouchedSecond, &expectedSecond, sizeof(untouchedSecond)) == 0,
		       "updating one registry object by identity changed another instance sharing its mesh handle");
		expect(firstObjectId != secondObjectId,
		       "shared geometry instances did not receive independent registry identities");
		registry.deinit();
	}
	void runDenseRemovalGenerationAndSceneReplacementModel()
	{
		FakeDevice device;
		GPUSceneRegistry registry;
		registry.init(&device);
		FakeCommandBuffer commandBuffer(&device);

		std::vector<GPUSceneRegistrationDesc> descriptions{makeDesc(0u), makeDesc(1u), makeDesc(2u)};
		const MeshHandle sharedMesh{.index = 31u, .generation = 9u};
		for (GPUSceneRegistrationDesc& desc : descriptions)
		{
			desc.meshHandle = sharedMesh;
		}
		const GPUSceneObjectHandle firstObject = registry.registerObject(descriptions[0]);
		const GPUSceneObjectHandle removedObject = registry.registerObject(descriptions[1]);
		const GPUSceneObjectHandle movedObject = registry.registerObject(descriptions[2]);
		registry.syncToGpu(commandBuffer);
		commandBuffer.executeCopies();
		expectRegistryContents(device, registry, descriptions);

		const GPUSceneRemoveResult removal = registry.removeObject(removedObject);
		expect(removal.removed && removal.removedObject == removedObject,
		       "middle registry object removal did not report the removed stable handle");
		expect(removal.hasDenseRemap()
		       && removal.movedObject == movedObject
		       && removal.removedDenseIndex == 1u
		       && removal.movedFromDenseIndex == 2u
		       && removal.movedToDenseIndex == 1u,
		       "middle removal did not publish the exact dense swap remap");
		uint32_t movedDenseIndex = UINT32_MAX;
		expect(registry.tryGetDenseIndex(movedObject, movedDenseIndex) && movedDenseIndex == 1u,
		       "moved registry object did not publish its compacted dense index");
		expect(!registry.updateTransform(
			removedObject, descriptions[1].transform, descriptions[1].boundsSphere),
		       "removed stable handle remained writable before slot reuse");

		commandBuffer.reset();
		registry.syncToGpu(commandBuffer);
		expect(commandBuffer.copies.size() == 2u,
		       "dense swap removal must upload one scene/cull replacement range");
		commandBuffer.executeCopies();
		std::vector<GPUSceneRegistrationDesc> compactedDescriptions{descriptions[0], descriptions[2]};
		expectRegistryContents(device, registry, compactedDescriptions);

		GPUSceneRegistrationDesc replacementDesc = makeDesc(3u);
		replacementDesc.meshHandle = sharedMesh;
		const GPUSceneObjectHandle replacementObject = registry.registerObject(replacementDesc);
		expect(replacementObject.index == removedObject.index
		       && replacementObject.generation != removedObject.generation,
		       "registry slot reuse did not advance the stable handle generation");
		expect(!registry.updateTransform(
			removedObject, descriptions[1].transform, descriptions[1].boundsSphere),
		       "stale stable handle updated a newly registered object after slot reuse");
		replacementDesc.transform =
			glm::translate(glm::mat4(1.0f), glm::vec3(-12.0f, 7.0f, 4.0f))
			* glm::rotate(glm::mat4(1.0f), glm::radians(29.0f), glm::vec3(0.0f, 1.0f, 0.0f))
			* glm::scale(glm::mat4(1.0f), glm::vec3(0.5f, 2.25f, 1.4f));
		replacementDesc.boundsSphere = glm::vec4(-11.0f, 8.0f, 5.0f, 4.5f);
		expect(registry.updateTransform(
			replacementObject, replacementDesc.transform, replacementDesc.boundsSphere),
		       "current generation handle could not update its registered object");
		expect(!registry.removeObject(removedObject).removed,
		       "stale stable handle removed the replacement object");

		commandBuffer.reset();
		registry.syncToGpu(commandBuffer);
		commandBuffer.executeCopies();
		compactedDescriptions.push_back(replacementDesc);
		expectRegistryContents(device, registry, compactedDescriptions);

		GPUSceneRegistrationDesc transientDesc = makeDesc(4u);
		const GPUSceneObjectHandle transientObject = registry.registerObject(transientDesc);
		expect(registry.updateTransform(
			transientObject, transientDesc.transform, transientDesc.boundsSphere),
		       "transient tail object update was rejected before removal");
		expect(registry.removeObject(transientObject).removed,
		       "transient tail object could not be removed");
		commandBuffer.reset();
		registry.syncToGpu(commandBuffer);
		expect(commandBuffer.copies.empty(),
		       "removed tail object left an out-of-range dirty upload behind");

		// A complete scene replacement invalidates every prior generation even if the
		// next scene immediately reuses the same numeric slot IDs.
		registry.clear();
		expect(registry.getObjectCount() == 0u,
		       "scene replacement retained old dense registry objects");
		GPUSceneRegistrationDesc newSceneDesc = makeDesc(9u);
		const GPUSceneObjectHandle newSceneObject = registry.registerObject(newSceneDesc);
		expect(newSceneObject.index == firstObject.index
		       && newSceneObject.generation != firstObject.generation,
		       "scene replacement reused a stale stable handle generation");
		expect(!registry.updateTransform(firstObject, descriptions[0].transform, descriptions[0].boundsSphere)
		       && !registry.updateTransform(movedObject, descriptions[2].transform, descriptions[2].boundsSphere)
		       && !registry.updateTransform(
			       replacementObject, replacementDesc.transform, replacementDesc.boundsSphere),
		       "old-scene handles remained writable after replacement");

		commandBuffer.reset();
		registry.syncToGpu(commandBuffer);
		commandBuffer.executeCopies();
		expectRegistryContents(device, registry, std::vector<GPUSceneRegistrationDesc>{newSceneDesc});
		registry.deinit();
	}
	void runMeshletMetadataForceRewriteModel()
	{
		FakeDevice device;
		GPUSceneRegistry registry;
		registry.init(&device);
		GPUMeshletBuffer meshletBuffer;
		meshletBuffer.init(&device);
		FakeCommandBuffer commandBuffer(&device);

		std::vector<GPUSceneRegistrationDesc> descriptions{makeDesc(0u), makeDesc(1u), makeDesc(2u)};
		std::vector<GPUSceneObjectHandle> owners;
		for (GPUSceneRegistrationDesc& desc : descriptions)
		{
			desc.meshHandle = MeshHandle{.index = 47u, .generation = 3u};
			owners.push_back(registry.registerObject(desc));
		}
		registry.syncToGpu(commandBuffer);
		commandBuffer.executeCopies();

		std::vector<shaderio::Meshlet> meshlets(3u);
		std::vector<shaderio::GPUCullObject> cullObjects(3u);
		for (uint32_t drawIndex = 0u; drawIndex < static_cast<uint32_t>(meshlets.size()); ++drawIndex)
		{
			meshlets[drawIndex].boundsSphere = glm::vec4(
				static_cast<float>(drawIndex), 1.0f, -2.0f, 3.0f + static_cast<float>(drawIndex));
			meshlets[drawIndex].indexOffset = drawIndex * 3u;
			meshlets[drawIndex].indexCount = 3u;
			meshlets[drawIndex].materialIndex = 10u + drawIndex;
			meshlets[drawIndex].objectIndex = drawIndex;
			meshlets[drawIndex].localIndex = drawIndex;
			cullObjects[drawIndex] = shaderio::GPUCullObject{
				.sphereCenterRadius = meshlets[drawIndex].boundsSphere,
				.indexCount = meshlets[drawIndex].indexCount,
				.firstIndex = meshlets[drawIndex].indexOffset,
				.vertexOffset = static_cast<int32_t>(drawIndex * 4u),
				.flags = shaderio::LGPUCullFlagFrustumCulling,
			};
		}
		const std::vector<uint32_t> meshletIndices{0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
		const GPUMeshletBuffer::UploadRecord initialUpload =
			meshletBuffer.uploadMeshlets(meshlets, meshletIndices, cullObjects);
		expect(initialUpload.meshletMetadata.firstElement == 0u
		       && initialUpload.meshletMetadata.elementCount == meshlets.size()
		       && initialUpload.meshletMetadata.byteOffset == 0u
		       && initialUpload.meshletMetadata.byteCount == sizeof(shaderio::Meshlet) * meshlets.size(),
		       "initial meshlet upload did not record the complete metadata write range");
		expect(initialUpload.indexGeometry.elementCount == meshletIndices.size()
		       && initialUpload.indexGeometry.byteCount == sizeof(uint32_t) * meshletIndices.size(),
		       "initial meshlet upload did not record the complete geometry write range");

		const rhi::BufferHandle meshletDataBuffer = meshletBuffer.getMeshletDataBuffer();
		const rhi::BufferHandle cullObjectBuffer = meshletBuffer.getMeshletCullObjectBuffer();
		const rhi::BufferHandle indexBuffer = meshletBuffer.getMeshletIndexBufferRHIHandle();
		const uint64_t meshletCapacityBytes = device.buffer(meshletDataBuffer).bytes.size();
		const uint64_t cullCapacityBytes = device.buffer(cullObjectBuffer).bytes.size();
		const uint64_t indexCapacityBytes = device.buffer(indexBuffer).bytes.size();
		const size_t meshletUsedBytes = sizeof(shaderio::Meshlet) * meshlets.size();
		const size_t cullUsedBytes = sizeof(shaderio::GPUCullObject) * cullObjects.size();
		std::fill(device.buffer(meshletDataBuffer).bytes.begin() + static_cast<std::ptrdiff_t>(meshletUsedBytes),
		          device.buffer(meshletDataBuffer).bytes.end(), std::byte{0x5a});
		std::fill(device.buffer(cullObjectBuffer).bytes.begin() + static_cast<std::ptrdiff_t>(cullUsedBytes),
		          device.buffer(cullObjectBuffer).bytes.end(), std::byte{0x6b});
		const std::vector<std::byte> meshletTailBefore(
			device.buffer(meshletDataBuffer).bytes.begin() + static_cast<std::ptrdiff_t>(meshletUsedBytes),
			device.buffer(meshletDataBuffer).bytes.end());
		const std::vector<std::byte> cullTailBefore(
			device.buffer(cullObjectBuffer).bytes.begin() + static_cast<std::ptrdiff_t>(cullUsedBytes),
			device.buffer(cullObjectBuffer).bytes.end());
		const std::vector<std::byte> geometryBefore = device.buffer(indexBuffer).bytes;

		const GPUSceneObjectHandle removedObject = owners[1];
		const GPUSceneRemoveResult removal = registry.removeObject(removedObject);
		expect(removal.hasDenseRemap() && removal.movedObject == owners[2]
		       && removal.movedFromDenseIndex == 2u && removal.movedToDenseIndex == 1u,
		       "meshlet metadata test did not produce the expected dense swap remap");
		for (uint32_t drawIndex = 0u; drawIndex < static_cast<uint32_t>(owners.size()); ++drawIndex)
		{
			if (owners[drawIndex] == removal.movedObject)
			{
				meshlets[drawIndex].objectIndex = removal.movedToDenseIndex;
			}
		}
		meshlets[1].objectIndex = UINT32_MAX;
		cullObjects[1] = {};

		const GPUMeshletBuffer::UploadRecord metadataRewrite = meshletBuffer.uploadMeshlets(
			meshlets,
			meshletIndices,
			cullObjects,
			GPUMeshletBuffer::MetadataUploadMode::forceFullRewrite);
		expect(metadataRewrite.forcedFullMetadataRewrite && !metadataRewrite.buffersRecreated,
		       "same-size metadata rewrite unexpectedly recreated meshlet buffers");
		expect(metadataRewrite.meshletMetadata.firstElement == 0u
		       && metadataRewrite.meshletMetadata.elementCount == meshlets.size()
		       && metadataRewrite.meshletMetadata.byteOffset == 0u
		       && metadataRewrite.meshletMetadata.byteCount == sizeof(shaderio::Meshlet) * meshlets.size(),
		       "same-size removal/remap did not record a full meshlet metadata write");
		expect(metadataRewrite.cullMetadata.firstElement == 0u
		       && metadataRewrite.cullMetadata.elementCount == cullObjects.size()
		       && metadataRewrite.cullMetadata.byteOffset == 0u
		       && metadataRewrite.cullMetadata.byteCount == sizeof(shaderio::GPUCullObject) * cullObjects.size(),
		       "same-size removal/remap did not record a full cull metadata write");
		expect(metadataRewrite.indexGeometry.elementCount == 0u
		       && metadataRewrite.indexGeometry.byteCount == 0u,
		       "metadata-only rewrite retransmitted unchanged meshlet index geometry");
		expect(meshletBuffer.getMeshletDataBuffer() == meshletDataBuffer
		       && meshletBuffer.getMeshletCullObjectBuffer() == cullObjectBuffer
		       && meshletBuffer.getMeshletIndexBufferRHIHandle() == indexBuffer,
		       "same-size metadata rewrite changed a meshlet buffer identity");
		expect(device.buffer(meshletDataBuffer).bytes.size() == meshletCapacityBytes
		       && device.buffer(cullObjectBuffer).bytes.size() == cullCapacityBytes
		       && device.buffer(indexBuffer).bytes.size() == indexCapacityBytes,
		       "same-size metadata rewrite changed meshlet buffer capacity");
		expect(std::vector<std::byte>(
			       device.buffer(meshletDataBuffer).bytes.begin() + static_cast<std::ptrdiff_t>(meshletUsedBytes),
			       device.buffer(meshletDataBuffer).bytes.end()) == meshletTailBefore
		       && std::vector<std::byte>(
			       device.buffer(cullObjectBuffer).bytes.begin() + static_cast<std::ptrdiff_t>(cullUsedBytes),
			       device.buffer(cullObjectBuffer).bytes.end()) == cullTailBefore,
		       "metadata rewrite wrote beyond its recorded byte ranges");
		expect(device.buffer(indexBuffer).bytes == geometryBefore,
		       "metadata rewrite changed GPU-visible meshlet index geometry");

		const shaderio::Meshlet firstGpuMeshlet =
			readElement<shaderio::Meshlet>(device, meshletDataBuffer, 0u);
		const shaderio::Meshlet tombstoneGpuMeshlet =
			readElement<shaderio::Meshlet>(device, meshletDataBuffer, 1u);
		const shaderio::Meshlet movedGpuMeshlet =
			readElement<shaderio::Meshlet>(device, meshletDataBuffer, 2u);
		expect(firstGpuMeshlet.objectIndex == 0u
		       && tombstoneGpuMeshlet.objectIndex == UINT32_MAX
		       && movedGpuMeshlet.objectIndex == removal.movedToDenseIndex,
		       "GPU-visible meshlet metadata retained stale tombstone or dense object indices");
		const shaderio::GPUCullObject tombstoneCullObject =
			readElement<shaderio::GPUCullObject>(device, cullObjectBuffer, 1u);
		const shaderio::GPUCullObject zeroCullObject{};
		expect(std::memcmp(&tombstoneCullObject, &zeroCullObject, sizeof(zeroCullObject)) == 0,
		       "GPU-visible tombstone cull metadata was not zeroed");

		commandBuffer.reset();
		registry.syncToGpu(commandBuffer);
		commandBuffer.executeCopies();
		expectRegistryContents(
			device, registry, std::vector<GPUSceneRegistrationDesc>{descriptions[0], descriptions[2]});

		std::vector<shaderio::GPUCullIndirectCommand> rawCommands(meshlets.size());
		uint32_t totalLiveCount = 0u;
		uint32_t visibleDrawCount = 0u;
		for (uint32_t drawIndex = 0u; drawIndex < static_cast<uint32_t>(rawCommands.size()); ++drawIndex)
		{
			const shaderio::Meshlet gpuMeshlet =
				readElement<shaderio::Meshlet>(device, meshletDataBuffer, drawIndex);
			if (gpuMeshlet.objectIndex == UINT32_MAX)
			{
				rawCommands[drawIndex] = {};
				continue;
			}
			++totalLiveCount;
			rawCommands[drawIndex] = shaderio::GPUCullIndirectCommand{
				.indexCount = gpuMeshlet.indexCount,
				.instanceCount = 1u,
				.firstIndex = gpuMeshlet.indexOffset,
				.vertexOffset = 0,
				.firstInstance = drawIndex,
			};
			++visibleDrawCount;
		}
		const uint32_t executableCommandCount = static_cast<uint32_t>(std::count_if(
			rawCommands.begin(), rawCommands.end(), [](const shaderio::GPUCullIndirectCommand& command)
			{
				return command.instanceCount != 0u;
			}));
		expect(totalLiveCount == 2u && visibleDrawCount == 2u
		       && executableCommandCount == visibleDrawCount,
		       "tombstone culling count did not match the GPU model's executable command count");
		expect(rawCommands[1].indexCount == 0u && rawCommands[1].instanceCount == 0u,
		       "tombstone GPU model retained an executable indirect command");

		meshletBuffer.deinit();
		registry.deinit();
	}
	void runConsecutiveSceneReplacementTopologyModel()
	{
		struct DrawIdentityModel
		{
			GPUSceneObjectHandle object{};
			uint32_t drawRecordIndex{UINT32_MAX};
			uint32_t denseObjectIndex{UINT32_MAX};
		};

		FakeDevice device;
		GPUSceneRegistry registry;
		registry.init(&device);
		FakeCommandBuffer commandBuffer(&device);
		std::vector<uint32_t> drawRecords;
		std::vector<uint32_t> meshletDraws;
		std::vector<DrawIdentityModel> identities;
		std::vector<uint32_t> opaqueBucket;
		std::vector<uint32_t> alphaBucket;
		std::vector<uint32_t> transparentBucket;

		const auto commitScene = [&](const std::vector<GPUSceneRegistrationDesc>& scene)
		{
			registry.clear();
			drawRecords.clear();
			meshletDraws.clear();
			identities.clear();
			opaqueBucket.clear();
			alphaBucket.clear();
			transparentBucket.clear();

			std::vector<GPUSceneObjectHandle> handles;
			for (uint32_t drawRecordIndex = 0; drawRecordIndex < scene.size(); ++drawRecordIndex)
			{
				const GPUSceneObjectHandle object = registry.registerObject(scene[drawRecordIndex]);
				uint32_t denseObjectIndex = UINT32_MAX;
				expect(registry.tryGetDenseIndex(object, denseObjectIndex),
				       "replacement scene object did not resolve a dense index");
				handles.push_back(object);
				drawRecords.push_back(drawRecordIndex);
				for (uint32_t localMeshletIndex = 0; localMeshletIndex < 2u; ++localMeshletIndex)
				{
					const uint32_t drawIndex = static_cast<uint32_t>(meshletDraws.size());
					meshletDraws.push_back(drawIndex);
					identities.push_back(DrawIdentityModel{
						.object = object,
						.drawRecordIndex = drawRecordIndex,
						.denseObjectIndex = denseObjectIndex,
					});
					if (drawRecordIndex % 3u == 0u)
					{
						opaqueBucket.push_back(drawIndex);
					}
					else if (drawRecordIndex % 3u == 1u)
					{
						alphaBucket.push_back(drawIndex);
					}
					else
					{
						transparentBucket.push_back(drawIndex);
					}
				}
			}
			return handles;
		};

		std::vector<GPUSceneRegistrationDesc> firstScene{makeDesc(0u), makeDesc(1u), makeDesc(2u)};
		const std::vector<GPUSceneObjectHandle> firstSceneHandles = commitScene(firstScene);
		expect(registry.getObjectCount() == 3u && drawRecords.size() == 3u
		       && identities.size() == 6u && !opaqueBucket.empty()
		       && !alphaBucket.empty() && !transparentBucket.empty(),
		       "first replacement topology was not fully populated");
		registry.syncToGpu(commandBuffer);
		commandBuffer.executeCopies();
		expectRegistryContents(device, registry, firstScene);
		commandBuffer.reset();

		std::vector<GPUSceneRegistrationDesc> secondScene{makeDesc(8u)};
		const std::vector<GPUSceneObjectHandle> secondSceneHandles = commitScene(secondScene);
		expect(registry.getObjectCount() == 1u
		       && drawRecords == std::vector<uint32_t>{0u}
		       && meshletDraws == std::vector<uint32_t>({0u, 1u})
		       && identities.size() == 2u
		       && opaqueBucket == std::vector<uint32_t>({0u, 1u})
		       && alphaBucket.empty() && transparentBucket.empty(),
		       "second commit appended onto old draw records, buckets, or meshlet topology");
		for (const DrawIdentityModel& identity : identities)
		{
			expect(identity.object == secondSceneHandles.front()
			       && identity.drawRecordIndex == 0u
			       && identity.denseObjectIndex == 0u,
			       "second commit retained an old-scene draw identity");
		}
		for (const GPUSceneObjectHandle oldHandle : firstSceneHandles)
		{
			expect(!registry.updateTransform(
				oldHandle, firstScene.front().transform, firstScene.front().boundsSphere),
			       "old-scene handle remained writable after a consecutive replacement commit");
		}

		commandBuffer.reset();
		registry.syncToGpu(commandBuffer);
		commandBuffer.executeCopies();
		expectRegistryContents(device, registry, secondScene);
		registry.deinit();
	}}

int main()
{
	try
	{
		runRegistryUploadModel();
		runSharedMeshInstanceIdentityModel();
		runDenseRemovalGenerationAndSceneReplacementModel();
		runMeshletMetadataForceRewriteModel();
		runConsecutiveSceneReplacementTopologyModel();
		std::cout << "gpu scene registry model tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "gpu scene registry model tests failed: " << error.what() << '\n';
		return 1;
	}
}
