#include "common/FrameDeferredValue.h"
#include "render/FlaxGIDebugModel.h"

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

	const demo::FlaxGIIndirectDispatch unlimited = demo::computeFlaxGIIndirectDispatch(513u, 1024u, 0u);
	passed &= expect(unlimited.activeProbeCount == 513u, "unlimited active probe count");
	passed &= expect(unlimited.trace == demo::FlaxGIDispatchDimensions{3u, 1u, 1u}, "trace rounds up by 256");
	passed &= expect(unlimited.distance == demo::FlaxGIDispatchDimensions{2052u, 1u, 1u}, "distance uses four groups per probe");
	passed &= expect(unlimited.irradiance == demo::FlaxGIDispatchDimensions{513u, 1u, 1u}, "irradiance uses one group per probe");

	const demo::FlaxGIIndirectDispatch budgeted = demo::computeFlaxGIIndirectDispatch(900u, 800u, 64u);
	passed &= expect(budgeted.activeProbeCount == 64u, "per-frame update budget clamps active probes");
	passed &= expect(budgeted.trace.x == 1u, "budgeted trace group count");
	passed &= expect(budgeted.distance.x == 256u, "budgeted distance group count");
	passed &= expect(budgeted.irradiance.x == 64u, "budgeted irradiance group count");

	const demo::FlaxGIIndirectDispatch empty = demo::computeFlaxGIIndirectDispatch(0u, 32768u, 64u);
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
