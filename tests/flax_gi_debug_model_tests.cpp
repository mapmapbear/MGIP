#include "common/FrameDeferredValue.h"
#include "render/FlaxGIDebugModel.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
	bool expect(bool condition, const char* message)
	{
		if(!condition)
		{
			std::cerr << "FAILED: " << message << '\n';
		}
		return condition;
	}
}

int main()
{
	bool passed = true;

	const demo::FlaxGIIndirectDispatch unlimited = demo::computeFlaxGIIndirectDispatch(513u, 1024u, 1024u);
	passed &= expect(unlimited.activeProbeCount == 513u, "classified active probe count is preserved");
	passed &= expect(unlimited.trace == demo::FlaxGIDispatchDimensions{3u, 1u, 1u}, "trace covers every classified active probe");
	passed &= expect(unlimited.distance == demo::FlaxGIDispatchDimensions{2052u, 1u, 1u}, "distance covers every classified active probe");
	passed &= expect(unlimited.irradiance == demo::FlaxGIDispatchDimensions{513u, 1u, 1u}, "irradiance covers every classified active probe");

	const demo::FlaxGIIndirectDispatch budgeted = demo::computeFlaxGIIndirectDispatch(900u, 800u, 64u);
	passed &= expect(budgeted.activeProbeCount == 800u, "allocation clamps the classified active count");
	passed &= expect(budgeted.trace.x == 1u, "budgeted trace group count");
	passed &= expect(budgeted.distance.x == 256u, "budgeted distance group count");
	passed &= expect(budgeted.irradiance.x == 64u, "budgeted irradiance group count");

	const demo::FlaxGIIndirectDispatch empty = demo::computeFlaxGIIndirectDispatch(0u, 32768u, 64u);
	passed &= expect(demo::computeFlaxGICascadeUpdateBudget(0u, 1024u, 0u, 4u) == 1024u,
	                 "zero global budget means a full logical-grid update");
	passed &= expect(demo::computeFlaxGICascadeUpdateBudget(65u, 1024u, 0u, 4u) == 17u,
	                 "budget remainder is assigned to the first cascade");
	passed &= expect(demo::computeFlaxGICascadeUpdateBudget(65u, 1024u, 1u, 4u) == 16u
	               && demo::computeFlaxGICascadeUpdateBudget(65u, 1024u, 2u, 4u) == 16u
	               && demo::computeFlaxGICascadeUpdateBudget(65u, 1024u, 3u, 4u) == 16u,
	                 "remaining cascades receive an even budget");
	passed &= expect(demo::computeFlaxGICascadeUpdateBudget(2u, 1024u, 0u, 4u) == 1u
	               && demo::computeFlaxGICascadeUpdateBudget(2u, 1024u, 1u, 4u) == 1u
	               && demo::computeFlaxGICascadeUpdateBudget(2u, 1024u, 2u, 4u) == 0u,
	                 "a small global budget does not become an unlimited cascade update");
	passed &= expect(demo::computeFlaxGICascadeUpdateBudget(64u, 1024u, 0u, 0u) == 0u,
	                 "zero cascades produce zero work");
	passed &= expect(empty.trace.x == 0u && empty.distance.x == 0u && empty.irradiance.x == 0u,
	                 "zero active probes produce zero indirect work");

	passed &= expect(demo::shouldExecuteFlaxGIStage(demo::FlaxGIDebugStage::classify,
	                                                demo::FlaxGIDebugStage::traceRays),
	                 "run-to-stage includes earlier stages");
	passed &= expect(!demo::shouldExecuteFlaxGIStage(demo::FlaxGIDebugStage::updateDistance,
	                                                 demo::FlaxGIDebugStage::traceRays),
	                 "run-to-stage excludes later stages");
	passed &= expect(demo::isFlaxGIActiveProbeCountValid(1u, 1024u),
	                 "non-zero bounded active probe count should be valid");
	passed &= expect(!demo::isFlaxGIActiveProbeCountValid(0u, 1024u),
	                 "zero active probes must fail when a probe volume exists");
	passed &= expect(!demo::isFlaxGIActiveProbeCountValid(1025u, 1024u),
	                 "active probe count must not exceed allocation");
	passed &= expect(demo::isNewFlaxGIDebugRequest(4u, 3u), "new request is detected");
	passed &= expect(!demo::isNewFlaxGIDebugRequest(4u, 4u), "consumed request is ignored");
	passed &= expect(!demo::isNewFlaxGIDebugRequest(0u, 9u), "zero request is never armed");

	const float distanceLimit = demo::computeFlaxGIDistanceLimit(1.5f, 96.0f);
	passed &= expect(std::abs(distanceLimit - 3.8971143f) < 1.0e-5f,
	                 "distance horizon is 1.5 probe-cell diagonals");
	passed &= expect(distanceLimit > std::sqrt(3.0f) * 1.5f,
	                 "all-miss horizon covers the complete interpolation cell");
	passed &= expect(std::abs(demo::computeFlaxGIDistanceLimit(1.5f, 2.0f) - 2.0f) < 1.0e-6f,
	                 "configured ray maximum caps the distance horizon");
	passed &= expect(std::abs(demo::computeFlaxGIRayStartOffset(1.5f, 0.25f) - 0.0625f)
	                   < 1.0e-6f,
	                 "ray start offset scales with SDF voxel size");

	demo::FlaxGIOutputState outputState{};
	passed &= expect(outputState.published() == demo::FlaxGIOutputSelection{0u, false},
	                 "FlaxGI output starts unpublished");
	passed &= expect(outputState.selectForFrame(true, false)
	                   == demo::FlaxGIOutputSelection{0u, true},
	                 "first full update publishes parity zero");
	outputState.publishPending();
	passed &= expect(outputState.published() == demo::FlaxGIOutputSelection{0u, true},
	                 "completed update publishes its write parity");
	passed &= expect(outputState.selectForFrame(false, false)
	                   == demo::FlaxGIOutputSelection{0u, true},
	                 "frozen frames remain pinned to the published parity");
	passed &= expect(outputState.selectForFrame(true, false)
	                   == demo::FlaxGIOutputSelection{1u, true},
	                 "resumed updates write the opposite history parity");
	outputState.publishPending();
	passed &= expect(outputState.published() == demo::FlaxGIOutputSelection{1u, true},
	                 "second completed update publishes parity one");
	outputState.invalidate();
	passed &= expect(outputState.selectForFrame(false, true)
	                   == demo::FlaxGIOutputSelection{1u, false},
	                 "reset disables sampling of cleared atlases");
	passed &= expect(outputState.selectForFrame(true, true)
	                   == demo::FlaxGIOutputSelection{0u, true},
	                 "a full update in the reset frame publishes the pending parity");
	outputState.publishPending();
	outputState.reset();
	passed &= expect(outputState.nextWriteParity == 0u && !outputState.published().valid,
	                 "resource rebuild resets output publication state");

	const demo::FlaxGIDistanceMoments blendedMoments =
		demo::blendFlaxGIDistanceMoments({1.0f, 1.0f}, {3.0f, 9.0f}, 0.5f);
	passed &= expect(std::abs(blendedMoments.first - 2.0f) < 1.0e-6f
	                   && std::abs(blendedMoments.second - 5.0f) < 1.0e-6f,
	                 "temporal history blends first and second raw moments");
	passed &= expect(std::abs(demo::computeFlaxGIDistanceVariance(blendedMoments) - 1.0f)
	                   < 1.0e-6f,
	                 "variance retains the between-mean temporal term");
	passed &= expect(demo::computeFlaxGIDistanceVariance({1.0f, 1.0f}) == 0.0f,
	                 "all-miss normalized moments remain fully visible");

	passed &= expect(demo::flaxGIDebugStageName(demo::FlaxGIDebugStage::traceRays) == "Trace Rays",
	                 "stage names remain stable for UI and captures");
	passed &= expect(demo::flaxGIDebugViewName(demo::FlaxGIDebugView::probeIrradiance) == "Probe Irradiance",
	                 "view names remain stable for overlays");

	demo::FrameDeferredValue<int> deferredConfig;
	passed &= expect(!deferredConfig.hasPending(), "deferred config starts empty");
	deferredConfig.defer(50);
	passed &= expect(deferredConfig.hasPending(), "UI edit queues config without applying it");
	deferredConfig.defer(75);
	const std::optional<int> appliedConfig = deferredConfig.consume();
	passed &= expect(appliedConfig.has_value() && *appliedConfig == 75,
	                 "latest UI edit is consumed at the next frame boundary");
	passed &= expect(!deferredConfig.hasPending() && !deferredConfig.consume().has_value(),
	                 "consuming pending config clears the queue exactly once");

	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
