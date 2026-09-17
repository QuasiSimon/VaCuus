// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaCuusEngine.h"

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The re-vendoring check for RmlUi Patch #8 (Source/ThirdParty/RmlUi/VENDORED_TAG.txt): a `data-for` view is
 * updated before the views on the rows it generated, so a shrink removes a row before anything reads it. The
 * mechanism lives in that entry and in the comment on DataViewFor's constructor; this header holds only what is
 * specific to the rig.
 *
 * THE OBSERVABLE IS THE ARRAY DEFINITION ITSELF. A row view runs its expression through the model
 * (DataModel::GetVariableInto, DataModel.cpp:316-323), and the model walks `Rows[i]` into this definition's
 * Child (DataModel.cpp:285-290). A view that runs for a row the shrink has already cut off is therefore exactly
 * one Child call past the end, and nothing else in the rig calls Child: DataViewFor::Update asks the container for
 * its Size only (DataViewDefault.cpp:526-531). RmlUi's "Could not get value from data variable" warning
 * (DataModel.cpp:321) marks the same event, but a warning does not fail an automation test, and its wording belongs
 * to the vendored SHA.
 *
 * WHY MANY ROWS AND MANY SHRINKS. Without the patch, the rows' views and the data-for view share one sort key, and
 * DataViews::Update orders them by std::sort after a sort by pointer value (DataView.cpp:98-104). Which of them
 * runs first is a matter of heap addresses, so one small shrink can pass by luck. The walk below removes 2910 row
 * views across six shrinks, and without the patch every one of them would have to run after the data-for view for
 * the test to pass.
 */
namespace VaCuusDataForOrderTest
{
static const char* GContextName = "vacuus_data_for_order_test";
static const char* GModelName = "order";

/**
 * Three views on the row element itself -- the shape of a clickable list row that carries its id and state as
 * attributes and classes -- and none inside it, so every row view has the data-for view's own sort key. No text:
 * the rig needs no font.
 */
static const char* GDocument = R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body data-model="order">
	<div id="rows"><div data-for="row : Rows" data-attr-value="row" data-attr-next="row + 1" data-class-high="row > 4"/></div>
</body>
</rml>)";

static constexpr int32 GViewsPerRow = 3;

/** `Rows` as the document sees it: an array of int32 that counts how each row lookup went. */
class FCountingRowsDefinition final : public Rml::VariableDefinition
{
public:
	FCountingRowsDefinition()
		: Rml::VariableDefinition(Rml::DataVariableType::Array)
	{
	}

	virtual int Size(void* Ptr) override
	{
		return static_cast<TArray<int32>*>(Ptr)->Num();
	}

	virtual Rml::DataVariable Child(void* Ptr, const Rml::DataAddressEntry& Address) override
	{
		TArray<int32>& Rows = *static_cast<TArray<int32>*>(Ptr);
		if (Address.index < 0)
		{
			// A name lookup (`Rows.size`); the document makes none.
			++NumNamedReads;
			return Rml::DataVariable();
		}

		if (Address.index >= Rows.Num())
		{
			++NumReadsPastEnd;
			return Rml::DataVariable();
		}

		++NumReadsInRange;
		return Rml::DataVariable(&RowDefinition, &Rows[Address.index]);
	}

	int32 NumReadsInRange = 0;
	int32 NumReadsPastEnd = 0;
	int32 NumNamedReads = 0;

private:
	Rml::ScalarDefinition<int32> RowDefinition;
};

/** The generated rows, in order. The template keeps its `data-for` and the rows do not (DataViewDefault.cpp:514-519),
 *  and the rows are inserted before it (:551), so every child of #rows without the attribute is a row. */
static TArray<Rml::Element*> GetRows(Rml::ElementDocument& Document)
{
	TArray<Rml::Element*> Rows;
	Rml::Element* Container = Document.GetElementById("rows");
	if (Container == nullptr)
	{
		return Rows;
	}

	for (int32 Index = 0; Index < Container->GetNumChildren(); ++Index)
	{
		Rml::Element* Child = Container->GetChild(Index);
		if (Child != nullptr && !Child->HasAttribute("data-for"))
		{
			Rows.Add(Child);
		}
	}
	return Rows;
}
}	 // namespace VaCuusDataForOrderTest

/**
 * A walk of grows and shrinks over one `data-for`. Each shrink must read no row past the end; each step must also
 * show the rows its array holds, with every binding evaluated, which is what keeps "nothing was read past the end"
 * from passing on a document that bound nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusDataForShrinkOrderTest, "VaCuus.Core.DataFor.ShrinkOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusDataForShrinkOrderTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusDataForOrderTest;

	// Declared before the library comes up, so both outlive the model that points into them.
	TArray<int32> Rows;
	FCountingRowsDefinition Definition;

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	FVaCuusEngine& Engine = FVaCuusEngine::Get();
	if (!TestTrue(TEXT("Initialized"), Engine.Initialize()))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Engine.Shutdown();
	};

	Rml::Context* Context = Rml::CreateContext(GContextName, Rml::Vector2i(400, 300));
	if (!TestNotNull(TEXT("Context"), Context))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Rml::RemoveContext(GContextName);
	};

	// Before the document: `data-model` is read once, when the body is parented (Element.cpp:2203-2215).
	Rml::DataModelConstructor Constructor = Context->CreateDataModel(GModelName);
	if (!TestTrue(TEXT("the model was created"), static_cast<bool>(Constructor))
		|| !TestTrue(TEXT("Rows bound"), Constructor.BindCustomDataVariable("Rows", Rml::DataVariable(&Definition, &Rows))))
	{
		return false;
	}
	Rml::DataModelHandle Model = Constructor.GetModelHandle();

	Rml::ElementDocument* Document = Context->LoadDocumentFromMemory(GDocument, "vacuus://data_for_order.rml");
	if (!TestNotNull(TEXT("Document"), Document))
	{
		return false;
	}
	Document->Show();
	Context->Update();

	// Grows and shrinks alternate, and each shrink cuts rows made by a different grow.
	const int32 Sizes[] = { 8, 2, 64, 5, 256, 0, 100, 1, 200, 50, 400, 0 };

	int32 NumRowViewsRemoved = 0;
	for (const int32 Size : Sizes)
	{
		const int32 Before = GetRows(*Document).Num();
		const FString Step = FString::Printf(TEXT("%d -> %d rows"), Before, Size);

		Rows.SetNum(Size);
		for (int32 Index = 0; Index < Size; ++Index)
		{
			Rows[Index] = Index;
		}

		const int32 InRangeBefore = Definition.NumReadsInRange;
		const int32 PastEndBefore = Definition.NumReadsPastEnd;

		Model.DirtyVariable("Rows");
		Context->Update();

		// THE ASSERTION.
		TestEqual(FString::Printf(TEXT("%s: no row view read past the end of the array"), *Step),
			Definition.NumReadsPastEnd - PastEndBefore, 0);

		const TArray<Rml::Element*> Shown = GetRows(*Document);
		if (!TestEqual(FString::Printf(TEXT("%s: rows shown"), *Step), Shown.Num(), Size))
		{
			return false;
		}

		// Every row's bindings hold its own value, whether the row is new or survived the step.
		for (int32 Index = 0; Index < Size; ++Index)
		{
			const FString Value = UTF8_TO_TCHAR(Shown[Index]->GetAttribute<Rml::String>("value", Rml::String()).c_str());
			const FString Next = UTF8_TO_TCHAR(Shown[Index]->GetAttribute<Rml::String>("next", Rml::String()).c_str());
			const bool bHigh = Shown[Index]->IsClassSet("high");
			if (Value != FString::FromInt(Index) || Next != FString::FromInt(Index + 1) || bHigh != (Index > 4))
			{
				AddError(FString::Printf(TEXT("%s: row %d shows value='%s' next='%s' high=%d"), *Step, Index, *Value, *Next,
					bHigh ? 1 : 0));
				return false;
			}
		}

		if (Size < Before)
		{
			NumRowViewsRemoved += (Before - Size) * GViewsPerRow;

			// The survivors were dirtied with the removed rows and read in the same update: the removed rows' views
			// were in that update too, and only their order decides whether they read past the end.
			TestTrue(FString::Printf(TEXT("%s: the surviving rows' views were evaluated"), *Step),
				Definition.NumReadsInRange - InRangeBefore >= Size * GViewsPerRow);
		}
	}

	TestEqual(TEXT("the document never looked a row up by name"), Definition.NumNamedReads, 0);
	AddInfo(FString::Printf(TEXT("%d row views removed across the walk, %d reads past the end, %d in range"),
		NumRowViewsRemoved, Definition.NumReadsPastEnd, Definition.NumReadsInRange));

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
