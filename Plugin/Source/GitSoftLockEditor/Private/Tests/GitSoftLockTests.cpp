#include "GitSoftLockEditorModule.h"
#include "Curves/CurveFloat.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSoftLockSaveTest, "GitSoftLock.Integration.SaveProtection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitSoftLockSaveTest::RunTest(const FString&)
{
    if (FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) != TEXT("GitSoftLockTestHost"))
    {
        AddError(TEXT("Run this integration test only in the isolated GitSoftLockTestHost project."));
        return false;
    }
    FString JSON;
    TSharedPtr<FJsonObject> Config;
    if (!FFileHelper::LoadFileToString(JSON, *(FPaths::ProjectSavedDir() / TEXT("GitSoftLock/test-connection.json"))) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JSON), Config))
    { AddError(TEXT("Missing local test server configuration")); return false; }

    FGitSoftLockConnection Alice;
    Alice.Url = Config->GetStringField(TEXT("url")); Alice.Repository = TEXT("test");
    Alice.Workspace = TEXT("alice-editor"); Alice.Token = Config->GetStringField(TEXT("alice")); Alice.Timeout = 1;
    FGitSoftLockConnection Bob = Alice;
    Bob.Workspace = TEXT("bob-editor"); Bob.Token = Config->GetStringField(TEXT("bob"));
    auto& Module = FModuleManager::LoadModuleChecked<FGitSoftLockEditorModule>(TEXT("GitSoftLockEditor"));
    Module.SetConnectionForTests(Alice);

    const FString Name = TEXT("/Game/SoftLockTests/Asset_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    UPackage* Package = CreatePackage(*Name);
    UCurveFloat* Asset = NewObject<UCurveFloat>(Package, *FPaths::GetBaseFilename(Name), RF_Public | RF_Standalone);
    Asset->FloatCurve.AddKey(0, 1);
    const FString Relative = TEXT("Content/") + Name.Mid(6) + TEXT(".uasset");
    const FString Filename = FPaths::ProjectDir() / Relative;
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    auto Save = [&](const FString& File) { return UPackage::SavePackage(Package, Asset, *File, Args); };
    TestTrue(TEXT("Owner can save a real Unreal package"), Save(Filename));
    const FString Before = GitSoftLockFingerprint(Filename);
    TestTrue(TEXT("Saved package exists"), Before != TEXT("missing") && !Before.IsEmpty());

    auto Claim = [&](const FGitSoftLockConnection& Conn, const FString& Path, const FString& Base) {
        auto Body = Conn.Body(); Body->SetStringField(TEXT("path"), Path); Body->SetStringField(TEXT("base"), Base);
        return Conn.Send(TEXT("claim"), Body);
    };
    auto Release = [&](const FGitSoftLockConnection& Conn, const FString& Path, const FString& Hash) {
        auto Owned = Claim(Conn, Path, Hash);
        if (!GitSoftLockGranted(Owned)) return false;
        auto Body = Conn.Body(); Body->SetStringField(TEXT("path"), Path);
        Body->SetStringField(TEXT("generation"), Owned->GetObjectField(TEXT("lock"))->GetStringField(TEXT("generation")));
        Body->SetStringField(TEXT("fingerprint"), Hash);
        return GitSoftLockGranted(Conn.Send(TEXT("release"), Body));
    };
    auto OldPredicate = Module.SwapPreviousSaveForTests(FCoreUObjectDelegates::FIsPackageOKToSaveDelegate::CreateLambda(
        [](UPackage*, const FString&, FOutputDevice*) { return false; }));
    TestFalse(TEXT("The prior engine predicate can still veto an owned package"), Save(Filename));
    Module.SwapPreviousSaveForTests(OldPredicate);
    const FString ForeignDestination = TEXT("Content/SoftLockTests/Reserved_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".uasset");
    TestTrue(TEXT("Peer reserves Save As destination"), GitSoftLockGranted(Claim(Bob, ForeignDestination, TEXT("missing"))));
    TestFalse(TEXT("Owned source cannot overwrite a foreign destination"), Save(FPaths::ProjectDir() / ForeignDestination));
    TestFalse(TEXT("Foreign destination stays absent"), IFileManager::Get().FileExists(*(FPaths::ProjectDir() / ForeignDestination)));
    TestTrue(TEXT("Release initial owner"), Release(Alice, Relative, Before));
    TestTrue(TEXT("Second teammate acquires lock"), GitSoftLockGranted(Claim(Bob, Relative, Before)));
    Asset->FloatCurve.AddKey(1, 99); Package->SetDirtyFlag(true);
    Module.TrackModified(Package);
    Module.AcknowledgeTesting(Relative);
    TestFalse(TEXT("Testing acknowledgement NEVER authorizes a conflicting save"), Save(Filename));
    TestEqual(TEXT("Conflicting save leaves original bytes unchanged"), GitSoftLockFingerprint(Filename), Before);
    TestTrue(TEXT("Denied save retains unsaved dirty state"), Package->IsDirty());
    FCanDeleteAssetResult CanDelete;
    FEditorDelegates::OnAssetsCanDelete.Broadcast({Asset}, CanDelete);
    TestFalse(TEXT("Editor deletion predicate rejects another owner's asset"), CanDelete.Get());
    const TArray<UObject*> RenameObjects{Asset};
    FOnPreDestructiveAssetActionArgs RenameArgs(RenameObjects, EDestructiveAssetActions::AssetRename);
    FEditorDelegates::OnPreDestructiveAssetActionWithArgs.Broadcast(RenameArgs);
    TestFalse(TEXT("Editor rename predicate rejects another owner's asset"), RenameArgs.Result.bSuccess);

    const FString Auto = FPaths::ProjectSavedDir() / TEXT("Autosaves/Test_Auto.uasset");
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Auto), true);
    Args.SaveFlags = SAVE_NoError | SAVE_FromAutosave;
    TestFalse(TEXT("Autosave of another owner's package is blocked"), Save(Auto));
    TestFalse(TEXT("Denied autosave creates no file"), IFileManager::Get().FileExists(*Auto));
    Args.SaveFlags = SAVE_NoError;
    const FString Copy = FPaths::ProjectContentDir() / TEXT("SoftLockTests/Copy.uasset");
    TestFalse(TEXT("Save As cannot bypass original package lock"), Save(Copy));
    TestFalse(TEXT("Denied Save As creates no file"), IFileManager::Get().FileExists(*Copy));

    FGitSoftLockConnection Offline = Alice; Offline.Url = TEXT("http://127.0.0.1:1");
    Module.SetConnectionForTests(Offline);
    TestFalse(TEXT("Unreachable service denies save"), Save(Filename));
    TestEqual(TEXT("Offline save leaves bytes unchanged"), GitSoftLockFingerprint(Filename), Before);
    Module.SetConnectionForTests(Alice);

    // Revert uses the editor package reload path; resolve the replacement UObject afterward.
    TestTrue(TEXT("Revert reloads local package"), Module.RevertPackage(Package));
    auto* Reloaded = LoadObject<UCurveFloat>(nullptr, *(Name + TEXT(".") + FPaths::GetBaseFilename(Name)));
    TestNotNull(TEXT("Asset still loads after revert"), Reloaded);
    if (Reloaded) TestEqual(TEXT("Testing curve key was discarded"), Reloaded->FloatCurve.GetNumKeys(), 1);
    TestEqual(TEXT("Revert never modifies disk content"), GitSoftLockFingerprint(Filename), Before);
    TestTrue(TEXT("Peer releases for next owner"), Release(Bob, Relative, Before));
    if (Reloaded)
    {
        Package = Reloaded->GetPackage(); Asset = Reloaded;
        TestTrue(TEXT("Saving resumes after peer releases"), Save(Filename));
    }

    const FString MapName = TEXT("/Game/SoftLockTests/Map_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    UPackage* MapPackage = CreatePackage(*MapName);
    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, *FPaths::GetBaseFilename(MapName), MapPackage);
    World->SetFlags(RF_Public | RF_Standalone);
    const FString MapRelative = TEXT("Content/") + MapName.Mid(6) + TEXT(".umap");
    const FString MapFile = FPaths::ProjectDir() / MapRelative;
    TestTrue(TEXT("Owner can save a real map"), UPackage::SavePackage(MapPackage, World, *MapFile, Args));
    const FString MapHash = GitSoftLockFingerprint(MapFile);
    TestTrue(TEXT("Owner releases map"), Release(Alice, MapRelative, MapHash));
    TestTrue(TEXT("Peer locks map"), GitSoftLockGranted(Claim(Bob, MapRelative, MapHash)));
    MapPackage->SetDirtyFlag(true);
    TestFalse(TEXT("Peer map cannot be saved"), UPackage::SavePackage(MapPackage, World, *MapFile, Args));
    TestEqual(TEXT("Map bytes remain unchanged"), GitSoftLockFingerprint(MapFile), MapHash);
    MapPackage->SetDirtyFlag(false);
    World->DestroyWorld(false);
    return true;
}

// A real conflict fixture for visual inspection. Never available on the production project.
static FAutoConsoleCommand GitSoftLockPreview(TEXT("GitSoftLock.TestPreview"),
    TEXT("Create a disposable conflicting curve and show the real edit/save notifications in GitSoftLockTestHost."),
    FConsoleCommandDelegate::CreateLambda([]() {
        if (FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) != TEXT("GitSoftLockTestHost")) return;
        FString JSON; TSharedPtr<FJsonObject> Config;
        if (!FFileHelper::LoadFileToString(JSON, *(FPaths::ProjectSavedDir() / TEXT("GitSoftLock/test-connection.json"))) ||
            !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JSON), Config)) return;
        FGitSoftLockConnection Alice;
        Alice.Url = Config->GetStringField(TEXT("url")); Alice.Repository = TEXT("test");
        Alice.Workspace = TEXT("alice-preview"); Alice.Token = Config->GetStringField(TEXT("alice"));
        FGitSoftLockConnection Bob = Alice; Bob.Workspace = TEXT("bob-preview"); Bob.Token = Config->GetStringField(TEXT("bob"));
        auto& Module = FModuleManager::LoadModuleChecked<FGitSoftLockEditorModule>(TEXT("GitSoftLockEditor"));
        Module.SetConnectionForTests(Alice);
        const FString Name = TEXT("/Game/SoftLockTests/LockedByBob_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const FString Path = TEXT("Content/") + Name.Mid(6) + TEXT(".uasset");
        const FString File = FPaths::ProjectDir() / Path;
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
        UPackage* Package = CreatePackage(*Name);
        UCurveFloat* Asset = NewObject<UCurveFloat>(Package, *FPaths::GetBaseFilename(Name), RF_Public | RF_Standalone);
        Asset->FloatCurve.AddKey(0, 1);
        FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
        if (!UPackage::SavePackage(Package, Asset, *File, Args)) return;
        auto Body = Alice.Body(); Body->SetStringField(TEXT("path"), Path); Body->SetStringField(TEXT("base"), GitSoftLockFingerprint(File));
        auto Owned = Alice.Send(TEXT("claim"), Body);
        if (!GitSoftLockGranted(Owned)) return;
        Body->SetStringField(TEXT("generation"), Owned->GetObjectField(TEXT("lock"))->GetStringField(TEXT("generation")));
        Body->SetStringField(TEXT("fingerprint"), GitSoftLockFingerprint(File));
        if (!GitSoftLockGranted(Alice.Send(TEXT("release"), Body))) return;
        Body->SetStringField(TEXT("workspace"), Bob.Workspace);
        if (!GitSoftLockGranted(Bob.Send(TEXT("claim"), Body))) return;
        Module.SetConnectionForTests(Alice);
        Package->SetDirtyFlag(true); Asset->FloatCurve.AddKey(1, 42);
        Module.TrackModified(Package);
        UPackage::SavePackage(Package, Asset, *File, Args);
    }));

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSoftLockPathTest, "GitSoftLock.Unit.PathBoundary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitSoftLockPathTest::RunTest(const FString&)
{
    FString Out;
    TestTrue(TEXT("Project plugin content is protected"), GitSoftLockRelativePath(TEXT("D:/Project"), TEXT("D:/Project/Plugins/P/Content/A.uasset"), Out));
    TestFalse(TEXT("Prefix sibling is outside checkout"), GitSoftLockRelativePath(TEXT("D:/Project"), TEXT("D:/ProjectOther/A.uasset"), Out));
    TestFalse(TEXT("Traversal escapes checkout"), GitSoftLockRelativePath(TEXT("D:/Project"), TEXT("D:/Project/../Other/A.uasset"), Out));
    TestFalse(TEXT("Text file is outside package protection"), GitSoftLockRelativePath(TEXT("D:/Project"), TEXT("D:/Project/A.txt"), Out));
    TestFalse(TEXT("Malformed service reply fails closed"), GitSoftLockGranted(MakeShared<FJsonObject>()));
    return true;
}
#endif
