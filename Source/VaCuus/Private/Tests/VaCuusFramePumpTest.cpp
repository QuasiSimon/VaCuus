// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusEngine.h"
#include "VaCuusSubsystem.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"

#include "VaCuusModelLayoutTestTypes.h"
#include "VaCuusModelTestHost.h"

#include "Engine/GameInstance.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * WHO PUBLISHES THE FRAME, AND WHY IT CANNOT ALWAYS BE Tick().
 *
 * UVaCuusSubsystem is an FTickableGameObject, so Tick() runs from FTickableGameObject::TickObjects
 * (Engine LevelTick.cpp:1821) -- before the frame's camera update (:1847) and before
 * FWorldDelegates::OnWorldTickEnd (:2061). A game that positions UI from the camera (a marker pinned to a
 * world object, a nameplate over a character) can only write those models after the camera is final, i.e.
 * after this subsystem has already published and pulsed. UpdateModel() only marks fields dirty, so those
 * writes waited for the NEXT frame's publish and the UI drew every such element one frame stale: invisible
 * at 60 fps, half a screen of separation at 15.
 *
 * TakeFramePump() hands the publish to that game, which then calls PumpUIFrame() once it has finished
 * writing. This asserts the hand-over in both directions, mostly on the one observable that needs no rendered
 * frame: NumOutstandingModelFields(), which drops to zero exactly when a publish happens. It then asserts the
 * other half of the contract -- what the hand-over must NOT take with it -- through the UI thread's frame
 * counter (no pulse while owned) and IsLoadPending() (status polling continues while owned).
 *
 * RESTORE THE BUG: make Tick() publish unconditionally again (delete the FramePumpOwners gate) and the
 * "tick yields" case below fails -- the field is published by the tick, before the game ever wrote it.
 */
namespace VaCuusFramePumpTest
{
using namespace VaCuusModelTest;

static const TCHAR* GModelName = TEXT("hud");

struct FFixture
{
	TStrongObjectPtr<UGameInstance> GameInstance;
	TStrongObjectPtr<UVaCuusSubsystem> Subsystem;
	UVaCuusView* View = nullptr;

	explicit FFixture(const TCHAR* ContextPrefix)
		: GameInstance(NewObject<UGameInstance>(GetTransientPackage()))
		, Subsystem(NewObject<UVaCuusSubsystem>(GameInstance.Get()))
	{
		TUniquePtr<FProbeHost> Owned = MakeUnique<FProbeHost>(ContextPrefix);
		View = Subsystem->CreateView(MoveTemp(Owned), FIntPoint(400, 300));
	}

	FFixture(const FFixture&) = delete;
	FFixture& operator=(const FFixture&) = delete;

	/** Marks one field dirty. Returns false if the write did not reach the model at all. */
	bool Write(int32 Ammo)
	{
		FVaCuusSamplerDefaultsModel Data;
		Data.Ammo = Ammo;
		View->UpdateModel(FName(GModelName), FVaCuusSamplerDefaultsModel::StaticStruct(), &Data);
		return View->NumOutstandingModelFields(FName(GModelName)) > 0;
	}

	int32 Outstanding() const { return View->NumOutstandingModelFields(FName(GModelName)); }

	void Destroy()
	{
		if (View != nullptr)
		{
			Subsystem->DestroyView(View);
		}
	}
};
}	 // namespace VaCuusFramePumpTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusFramePumpTest, "VaCuus.Model.FramePump",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusFramePumpTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusFramePumpTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to create a view against"));
		return true;
	}

	// Same precondition as the other view tests: a live PIE session owns RmlUi already, and this builds a
	// second FVaCuusUIThread. Init() would refuse and the run would prove nothing about the pump.
	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	FFixture Fixture(TEXT("FramePump"));
	if (!TestNotNull(TEXT("view"), Fixture.View))
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		Fixture.Destroy();
	};

	// BindModel takes the name as a string (it reaches RmlUi), the rest of the API as an FName.
	if (!TestTrue(TEXT("model bound"), Fixture.View->BindModel(FString(GModelName), FVaCuusSamplerDefaultsModel::StaticStruct())))
	{
		return false;
	}

	// The bind is completed BY THE UI THREAD, and until it has, nothing publishes at all -- outstanding fields
	// just accumulate, which is that failure's own observable. Everything below measures publishing, so the view
	// has to reach its steady state first.
	if (!TestTrue(TEXT("frames ran for the bind"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	Fixture.Subsystem->Tick(0.016f);
	if (!TestTrue(TEXT("frames ran for the initial publish"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	if (!TestEqual(TEXT("the view starts with nothing outstanding"), Fixture.Outstanding(), 0))
	{
		return false;
	}

	// Every step below runs UI frames after the game-thread call, because a field stops being outstanding when
	// the UI thread acknowledges it, not when it is handed over. That makes the negative cases stronger too: a
	// field still outstanding AFTER the frames ran was never published, rather than merely still in flight.
	int32 Ammo = 0;
	const auto WriteTickAndSettle = [&](bool bPump) -> bool
	{
		++Ammo;
		if (!Fixture.Write(Ammo))
		{
			return false;
		}

		if (bPump)
		{
			Fixture.Subsystem->PumpUIFrame();
		}
		else
		{
			Fixture.Subsystem->Tick(0.016f);
		}

		return RunFrames(*UIThread, 3);
	};

	// BASELINE: with nobody owning the pump, the subsystem's own tick publishes, exactly as it always has.
	if (!TestTrue(TEXT("baseline frames ran"), WriteTickAndSettle(/*bPump*/ false)))
	{
		return false;
	}
	TestEqual(TEXT("an unowned pump publishes from Tick()"), Fixture.Outstanding(), 0);

	// THE HAND-OVER: an owned pump makes Tick() leave the writes alone, because the owner has not written this
	// frame's models yet -- it writes them after the camera, later in the frame.
	Fixture.Subsystem->TakeFramePump();
	if (!TestTrue(TEXT("owned frames ran"), WriteTickAndSettle(/*bPump*/ false)))
	{
		return false;
	}
	TestNotEqual(TEXT("an owned pump is NOT published by Tick()"), Fixture.Outstanding(), 0);

	// And the owner's own pump is what gets it out -- including the write the tick just refused.
	Fixture.Subsystem->PumpUIFrame();
	if (!TestTrue(TEXT("pump frames ran"), RunFrames(*UIThread, 3)))
	{
		return false;
	}
	TestEqual(TEXT("PumpUIFrame() publishes"), Fixture.Outstanding(), 0);

	// OWNERSHIP IS COUNTED: split screen is two local players against one subsystem, and one of them going away
	// must not hand the pump back while the other still writes late in the frame.
	Fixture.Subsystem->TakeFramePump();
	Fixture.Subsystem->ReleaseFramePump();
	if (!TestTrue(TEXT("two-owner frames ran"), WriteTickAndSettle(/*bPump*/ false)))
	{
		return false;
	}
	TestNotEqual(TEXT("one owner left means Tick() still yields"), Fixture.Outstanding(), 0);

	// AND IT COMES BACK: the last release returns the pump to the tick, so a game that stops driving it does not
	// leave the UI frozen. The field the tick refused above is published by this one.
	Fixture.Subsystem->ReleaseFramePump();
	Fixture.Subsystem->Tick(0.016f);
	if (!TestTrue(TEXT("release frames ran"), RunFrames(*UIThread, 3)))
	{
		return false;
	}
	TestEqual(TEXT("the last release returns the pump to Tick()"), Fixture.Outstanding(), 0);

	// An unbalanced release is a caller bug, but it must not drive the count negative and leave the tick
	// convinced someone else publishes -- that would be a permanently frozen UI.
	Fixture.Subsystem->ReleaseFramePump();
	if (!TestTrue(TEXT("over-release frames ran"), WriteTickAndSettle(/*bPump*/ false)))
	{
		return false;
	}
	TestEqual(TEXT("an over-release does not leave the tick yielding"), Fixture.Outstanding(), 0);

	// WHAT THE HAND-OVER MUST **NOT** TAKE WITH IT. The publish and the pulse move; everything else Tick() does
	// stays. Without the next two checks the cheapest possible reading of this feature -- an early return at the
	// top of Tick() when the pump is owned -- passes every assertion above while silently killing status polling,
	// the write router's drain and the deferred style/texture releases.
	Fixture.Subsystem->TakeFramePump();

	// Drain any wake still owed from the step above first, so that what is measured next is this Tick() and not
	// the tail of the previous one.
	if (!TestTrue(TEXT("settled before measuring the pulse"), RunFrames(*UIThread, 2)))
	{
		return false;
	}

	const uint64 FramesBeforeOwnedTick = UIThread->GetFrameCount();
	Fixture.Subsystem->Tick(0.016f);
	TestFalse(TEXT("Tick() does not wake the UI thread while the pump is owned"),
		UIThread->WaitForFrameCount(FramesBeforeOwnedTick + 1, 0.25));

	// Status polling is not part of the hand-over either, and the observable for it has to be one that moves AT
	// THE POLL. Two nearby ones do not: IsLoadPending() compares serials the UI thread stores directly, so it
	// answers correctly whether or not PollStatus() ever ran, and GetSnapshot() never moves in this fixture
	// because the probe host publishes frames without publishing an interactive snapshot. OnLoadCompleted does:
	// PollStatus() is the only place that broadcasts it.
	int32 NumLoadBroadcasts = 0;
	const FDelegateHandle LoadHandle = Fixture.View->OnLoadCompleted.AddLambda(
		[&NumLoadBroadcasts](UVaCuusView*, bool) { ++NumLoadBroadcasts; });

	ON_SCOPE_EXIT
	{
		Fixture.View->OnLoadCompleted.Remove(LoadHandle);
	};

	Fixture.View->LoadDocumentFromMemory(GDocument);
	if (!TestTrue(TEXT("frames ran for the load"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	TestEqual(TEXT("a finished load is not announced without a poll"), NumLoadBroadcasts, 0);

	Fixture.Subsystem->Tick(0.016f);
	TestEqual(TEXT("Tick() still polls view status while the pump is owned"), NumLoadBroadcasts, 1);

	// ONE PUMP PUBLISHES EVERY VIEW, which is the whole reason ownership is a count on the subsystem rather than
	// a flag on a view: split screen is two local players against one game-instance subsystem, and the pump one
	// of them calls has to carry the other's writes too.
	UVaCuusView* SecondView = Fixture.Subsystem->CreateView(MakeUnique<FProbeHost>(TEXT("FramePumpB")), FIntPoint(400, 300));
	if (!TestNotNull(TEXT("second view"), SecondView))
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		Fixture.Subsystem->DestroyView(SecondView);
	};

	if (!TestTrue(TEXT("second model bound"),
			SecondView->BindModel(FString(GModelName), FVaCuusSamplerDefaultsModel::StaticStruct())))
	{
		return false;
	}

	Fixture.Subsystem->PumpUIFrame();
	if (!TestTrue(TEXT("frames ran for the second bind"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	++Ammo;
	FVaCuusSamplerDefaultsModel SecondData;
	SecondData.Ammo = Ammo;
	SecondView->UpdateModel(FName(GModelName), FVaCuusSamplerDefaultsModel::StaticStruct(), &SecondData);

	if (!TestTrue(TEXT("the owning view has something to publish"), Fixture.Write(Ammo)))
	{
		return false;
	}

	if (!TestTrue(TEXT("the other view has something to publish"),
			SecondView->NumOutstandingModelFields(FName(GModelName)) > 0))
	{
		return false;
	}

	Fixture.Subsystem->PumpUIFrame();
	if (!TestTrue(TEXT("frames ran for the shared pump"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	TestEqual(TEXT("one pump publishes the owning view"), Fixture.Outstanding(), 0);
	TestEqual(TEXT("one pump publishes the other view too"),
		SecondView->NumOutstandingModelFields(FName(GModelName)), 0);

	Fixture.Subsystem->ReleaseFramePump();

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
