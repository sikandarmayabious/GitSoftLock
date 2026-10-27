#include "GitSoftLockEditorModule.h"
#include "GitSoftLockSettings.h"
#include "Async/Async.h"
#include "Editor.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "Serialization/JsonSerializer.h"
#include "ToolMenus.h"
#include "UObject/Package.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogGitSoftLock, Log, All);
static const FName GitSoftLockTab(TEXT("GitSoftLock"));

static FString QuoteGit(const FString& Text)
{
    // Never invoke a shell. Reject characters that can corrupt Windows argument quoting.
    if (Text.Contains(TEXT("\"")) || Text.Contains(TEXT("\n")) || Text.Contains(TEXT("\r"))) return FString();
    return TEXT("\"") + Text + TEXT("\"");
}

void FGitSoftLockEditorModule::StartupModule()
{
    const auto* Settings = GetDefault<UGitSoftLockSettings>();
    bEnabled = Settings->bEnabled;
    bAutoRelease = Settings->bAutoReleasePublishedFiles;
    GitExecutable = Settings->GitExecutable;
    Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    FString GitRoot, Errors;
    int32 Code = -1;
    FPlatformProcess::ExecProcess(*GitExecutable, *(TEXT("-C ") + QuoteGit(Root) + TEXT(" rev-parse --show-toplevel")), &Code, &GitRoot, &Errors);
    if (Code == 0) Root = GitRoot.TrimStartAndEnd();
    FPaths::NormalizeDirectoryName(Root);

    Connection.Url = Settings->ServerUrl;
    Connection.Url.RemoveFromEnd(TEXT("/"));
    Connection.Repository = Settings->RepositoryId;
    Connection.Timeout = FMath::Clamp(Settings->RequestTimeoutSeconds, 1.0f, 10.0f);
    const FString LocalDir = FPaths::ProjectSavedDir() / TEXT("GitSoftLock");
    IFileManager::Get().MakeDirectory(*LocalDir, true);
    const FString ClientFile = LocalDir / TEXT("client.json");
    FString JSON;
    TSharedPtr<FJsonObject> Local;
    if (FFileHelper::LoadFileToString(JSON, *ClientFile))
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JSON), Local);
    if (Local)
    {
        Local->TryGetStringField(TEXT("token"), Connection.Token);
        Local->TryGetStringField(TEXT("workspace"), Connection.Workspace);
    }
    // Credentials are provisioned by client.py; never write secrets to project Config.
    if (Connection.Workspace.IsEmpty())
    {
        const FString WorkspaceFile = LocalDir / TEXT("workspace.txt");
        FFileHelper::LoadFileToString(Connection.Workspace, *WorkspaceFile);
        Connection.Workspace.TrimStartAndEndInline();
        if (Connection.Workspace.IsEmpty())
        {
            Connection.Workspace = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
            FFileHelper::SaveStringToFile(Connection.Workspace, *WorkspaceFile);
        }
    }
    if (bEnabled)
    {
        // Keep an OS file handle that denies another writer in this same checkout.
        WorkspaceGuard.Reset(IFileManager::Get().CreateFileWriter(*(LocalDir / TEXT("editor.guard")), FILEWRITE_AllowRead));
        if (!WorkspaceGuard || Code != 0)
        {
            Connection.Token.Reset();
            UE_LOG(LogGitSoftLock, Warning, TEXT("Protection blocked: checkout is already open or Git root could not be verified."));
        }
    }

    PreviousSave = FCoreUObjectDelegates::IsPackageOKToSaveDelegate;
    FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindRaw(this, &FGitSoftLockEditorModule::CanSave);
    SaveHandle = FCoreUObjectDelegates::IsPackageOKToSaveDelegate.GetHandle();
    FCoreUObjectDelegates::OnEndLoadPackage.AddRaw(this, &FGitSoftLockEditorModule::RememberLoaded);
    ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda([this](UObject* Object) {
        if (Object && IsInGameThread()) TrackModified(Object->GetPackage());
    });
    DirtyHandle = UPackage::PackageMarkedDirtyEvent.AddLambda([this](UPackage* Package, bool) { TrackModified(Package); });
    SavedHandle = UPackage::PackageSavedWithContextEvent.AddLambda([this](const FString&, UPackage* Package, FObjectPostSaveContext) {
        FString Path;
        if (PackagePath(Package, Path))
        {
            LoadedTimes.Add(Path, IFileManager::Get().GetTimeStamp(*(Root / Path)));
            Baselines.Remove(Path);
        }
    });
    DeleteHandle = FEditorDelegates::OnAssetsCanDelete.AddLambda([this](const TArray<UObject*>& Objects, FCanDeleteAssetResult& Result) {
        if (!bEnabled) return;
        for (UObject* Object : Objects)
        {
            FString Path;
            UPackage* Package = Object ? Object->GetPackage() : nullptr;
            if (PackagePath(Package, Path)) Result.Set(CanSave(Package, Root / Path, nullptr));
        }
    });
    DestructiveHandle = FEditorDelegates::OnPreDestructiveAssetActionWithArgs.AddLambda([this](FOnPreDestructiveAssetActionArgs& Args) {
        if (!bEnabled || !Args.Result.bSuccess) return;
        for (int32 Index = 0; Index < Args.Objects.Num(); ++Index)
        {
            UPackage* Package = Args.Objects[Index] ? Args.Objects[Index]->GetPackage() : nullptr;
            FString Path;
            if (!PackagePath(Package, Path)) continue;
            OriginalPaths.FindOrAdd(Package, Path);
            FString Destination = Root / Path;
            if (Args.Destinations.IsValidIndex(Index))
            {
                if (!FPackageName::TryConvertLongPackageNameToFilename(Args.Destinations[Index].GetPackageName().ToString(), Destination,
                    Package->ContainsMap() ? TEXT(".umap") : TEXT(".uasset")))
                { Args.Result.bSuccess = false; Args.Result.ErrorMessage = TEXT("Git Soft Lock: destination could not be resolved."); return; }
            }
            if (!CanSave(Package, Destination, nullptr))
            {
                Args.Result.bSuccess = false;
                Args.Result.ErrorMessage = TEXT("Git Soft Lock blocked this operation. See the ownership notification.");
                return;
            }
        }
    });
    Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FGitSoftLockEditorModule::Tick), 0.2f);
    if (!IsRunningCommandlet())
    {
        FGlobalTabmanager::Get()->RegisterNomadTabSpawner(GitSoftLockTab,
            FOnSpawnTab::CreateRaw(this, &FGitSoftLockEditorModule::SpawnDashboard))
            .SetDisplayName(FText::FromString(TEXT("Git Soft Lock")));
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FGitSoftLockEditorModule::RegisterMenus));
        Deferred.Add([this]() {
            if (!bEnabled || !Connection.IsValid())
            {
                FNotificationInfo Info(FText::FromString(bEnabled ?
                    TEXT("Git Soft Lock: saves blocked. Configure server and personal credentials, then restart.") :
                    TEXT("Git Soft Lock protection is OFF. Configure it in Project Settings > Plugins > Git Soft Lock.")));
                Info.ExpireDuration = 15;
                FSlateNotificationManager::Get().AddNotification(Info);
            }
        });
    }
}

void FGitSoftLockEditorModule::ShutdownModule()
{
    bStopping = true;
    FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
    FCoreUObjectDelegates::OnEndLoadPackage.RemoveAll(this);
    FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);
    UPackage::PackageMarkedDirtyEvent.Remove(DirtyHandle);
    UPackage::PackageSavedWithContextEvent.Remove(SavedHandle);
    FEditorDelegates::OnAssetsCanDelete.Remove(DeleteHandle);
    FEditorDelegates::OnPreDestructiveAssetActionWithArgs.Remove(DestructiveHandle);
    if (FCoreUObjectDelegates::IsPackageOKToSaveDelegate.GetHandle() == SaveHandle)
        FCoreUObjectDelegates::IsPackageOKToSaveDelegate = PreviousSave;
    for (auto& Request : Requests) { Request->OnProcessRequestComplete().Unbind(); Request->CancelRequest(); }
    if (ReleaseTask.IsValid()) ReleaseTask.Wait();
    for (auto& Notice : Notices) if (auto Item = Notice.Value.Pin()) Item->ExpireAndFadeout();
    if (!IsRunningCommandlet())
    {
        UToolMenus::UnRegisterStartupCallback(this);
        UToolMenus::UnregisterOwner(this);
        FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(GitSoftLockTab);
    }
    WorkspaceGuard.Reset();
}

bool FGitSoftLockEditorModule::PackagePath(UPackage* Package, FString& OutPath) const
{
    if (!Package || Package == GetTransientPackage() || Package->HasAnyFlags(RF_Transient) || Package->HasAnyPackageFlags(PKG_PlayInEditor | PKG_CompiledIn)) return false;
    FString Filename;
    if (!FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename,
        Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension())) return false;
    if (!GitSoftLockRelativePath(Root, Filename, OutPath)) return false;
    // Never acquire locks for engine, autosave copies, tests' generated output, or DDC packages.
    return !OutPath.StartsWith(TEXT("Saved/"), ESearchCase::IgnoreCase) && !OutPath.StartsWith(TEXT("Intermediate/"), ESearchCase::IgnoreCase);
}

void FGitSoftLockEditorModule::RememberLoaded(const FEndLoadPackageContext& Context)
{
    if (!bEnabled || !IsInGameThread()) return;
    for (UPackage* Package : Context.LoadedPackages)
    {
        FString Path;
        if (PackagePath(Package, Path))
        {
            LoadedTimes.Add(Path, IFileManager::Get().GetTimeStamp(*(Root / Path)));
            Baselines.Remove(Path);
            OriginalPaths.Remove(Package);
        }
    }
}

FString FGitSoftLockEditorModule::Baseline(UPackage* Package, const FString& Path)
{
    const FString File = Root / Path;
    if (const FDateTime* Loaded = LoadedTimes.Find(Path))
        if (*Loaded != IFileManager::Get().GetTimeStamp(*File)) return FString();
    if (const FString* Found = Baselines.Find(Path)) return *Found;
    const FString Hash = GitSoftLockFingerprint(File);
    if (!Hash.IsEmpty()) Baselines.Add(Path, Hash);
    return Hash;
}

void FGitSoftLockEditorModule::TrackModified(UPackage* Package)
{
    if (!bEnabled || bReverting || !IsInGameThread()) return;
    FString Path;
    if (!PackagePath(Package, Path)) return;
    Packages.Add(Path, Package);
    OriginalPaths.FindOrAdd(Package, Path);
    if (Testing.Contains(Path) || PendingClaims.Contains(Path)) return;
    const auto* Lock = Locks.Find(Path.ToLower());
    bool bMine = false;
    if (Lock && (*Lock)->TryGetBoolField(TEXT("mine"), bMine) && bMine) return;
    Claim(Package, Path);
}

void FGitSoftLockEditorModule::Claim(UPackage* Package, const FString& Path)
{
    const FString Base = Baseline(Package, Path);
    if (!Connection.IsValid() || Base.IsEmpty()) { QueueWarning(Package, Path, nullptr, false); return; }
    auto Body = Connection.Body();
    Body->SetStringField(TEXT("path"), Path);
    Body->SetStringField(TEXT("base"), Base);
    auto Request = Connection.Request(TEXT("claim"), Body);
    PendingClaims.Add(Path);
    const TWeakObjectPtr<UPackage> WeakPackage(Package);
    Request->OnProcessRequestComplete().BindLambda([this, WeakPackage, Path](FHttpRequestPtr, FHttpResponsePtr Response, bool) {
        if (bStopping) return;
        PendingClaims.Remove(Path);
        auto Reply = FGitSoftLockConnection::Parse(Response);
        const TSharedPtr<FJsonObject>* Lock = nullptr;
        if (Reply && Reply->TryGetObjectField(TEXT("lock"), Lock)) Locks.Add(Path.ToLower(), *Lock);
        if (!GitSoftLockGranted(Reply)) QueueWarning(WeakPackage.Get(), Path, Reply, false);
    });
    Requests.Add(Request);
    if (!Request->ProcessRequest()) { PendingClaims.Remove(Path); QueueWarning(Package, Path, nullptr, false); }
}

bool FGitSoftLockEditorModule::CanSave(UPackage* Package, const FString& Filename, FOutputDevice* Error)
{
    if (PreviousSave.IsBound() && !PreviousSave.Execute(Package, Filename, Error)) return false;
    if (!bEnabled) return true;
    FString Source;
    const bool bProjectPackage = PackagePath(Package, Source);
    FString Destination;
    const bool bProjectDestination = GitSoftLockRelativePath(Root, Filename, Destination) &&
        !Destination.StartsWith(TEXT("Saved/"), ESearchCase::IgnoreCase) && !Destination.StartsWith(TEXT("Intermediate/"), ESearchCase::IgnoreCase);
    if (!bProjectPackage && !bProjectDestination) return true;
    // Never touch UObject/Slate/maps from a worker-thread save. Deny instead of bypassing.
    if (!IsInGameThread()) return false;
    TArray<FString> Paths;
    if (const FString* Original = OriginalPaths.Find(Package)) Paths.AddUnique(*Original);
    if (bProjectPackage) Paths.AddUnique(Source);
    if (bProjectDestination) Paths.AddUnique(Destination);
    for (const FString& Path : Paths)
    {
        auto Body = Connection.Body();
        Body->SetStringField(TEXT("path"), Path);
        const FString Base = Baseline(Package, Path);
        Body->SetStringField(TEXT("base"), Base);
        // Fresh server authorization on EVERY write. Testing/cache/presence never authorize saving.
        // A Save All during an outage must not incur a full timeout for every package.
        // This caches DENIAL only; success is always verified with the service.
        auto Reply = Base.IsEmpty() || FPlatformTime::Seconds() < NetworkFailureUntil ? nullptr : Connection.Send(TEXT("claim"), Body);
        if (!Reply) NetworkFailureUntil = FPlatformTime::Seconds() + 2.0;
        const TSharedPtr<FJsonObject>* Lock = nullptr;
        bool bMine = false;
        const bool bAuthorized = GitSoftLockGranted(Reply) && Reply->TryGetObjectField(TEXT("lock"), Lock) &&
            (*Lock)->TryGetBoolField(TEXT("mine"), bMine) && bMine && (*Lock)->GetStringField(TEXT("workspace")) == Connection.Workspace;
        if (!bAuthorized)
        {
            // FSavePackageArgs may supply GError; writing even a warning to that device
            // can terminate the process. A policy denial is an ordinary, recoverable event.
            UE_LOG(LogGitSoftLock, Display, TEXT("Blocked save: %s"), *Path);
            QueueWarning(Package, Path, Reply, true);
            return false;
        }
        Locks.Add(Path.ToLower(), *Lock);
        Packages.Add(Path, Package);
    }
    return true;
}

void FGitSoftLockEditorModule::QueueWarning(UPackage* Package, const FString& Path, const TSharedPtr<FJsonObject>& Reply, bool bSave)
{
    FString Reason = TEXT("Lock ownership could not be verified. Check the connection, credentials, and whether the file changed on disk; reload if needed.");
    const TSharedPtr<FJsonObject>* Lock = nullptr;
    if (Reply && Reply->TryGetObjectField(TEXT("lock"), Lock))
    {
        FString Name;
        if ((*Lock)->TryGetStringField(TEXT("name"), Name)) Reason = TEXT("This file is locked by ") + Name + TEXT(".");
    }
    else if (Reply) Reply->TryGetStringField(TEXT("message"), Reason);
    if (IsRunningCommandlet()) { UE_LOG(LogGitSoftLock, Display, TEXT("%s: %s"), *Path, *Reason); return; }
    // Defer UI and reload until the active transaction/SavePackage call has unwound.
    Deferred.Add([this, Weak = TWeakObjectPtr<UPackage>(Package), Path, Reason, bSave]() { ShowWarning(Weak, Path, Reason, bSave); });
}

void FGitSoftLockEditorModule::ShowWarning(TWeakObjectPtr<UPackage> Package, FString Path, FString Reason, bool bSave)
{
    const FString Key = (bSave ? TEXT("save:") : TEXT("edit:")) + Path;
    if ((!bSave && Testing.Contains(Path)) || (Notices.Contains(Key) && Notices[Key].IsValid())) return;
    FNotificationInfo Info(FText::FromString(Path + TEXT("\n") + Reason + (bSave ? TEXT("\nSave blocked. No package was written.") : TEXT("\nTesting changes may remain in memory, but saving requires ownership."))));
    Info.bFireAndForget = false;
    Info.bUseThrobber = false;
    Info.bUseSuccessFailIcons = false;
    Info.bUseLargeFont = false;
    Info.WidthOverride = 520;
    // Buttons default to visible in Pending, while notifications otherwise start in None.
    Info.DefaultState = SNotificationItem::CS_Pending;
    Info.ButtonDetails.Emplace(FText::FromString(bSave ? TEXT("Don't save") : TEXT("I know, just doing testing")),
        FText::FromString(TEXT("Keep the unsaved changes in memory.")), FSimpleDelegate::CreateLambda([this, Path, Key, bSave]() {
            if (!bSave) AcknowledgeTesting(Path);
            Dismiss(Key);
        }));
    Info.ButtonDetails.Emplace(FText::FromString(bSave ? TEXT("Revert changes") : TEXT("Revert")),
        FText::FromString(TEXT("Discard this package's unsaved changes and reload its local disk version.")),
        FSimpleDelegate::CreateLambda([this, Package, Key]() {
            Deferred.Add([this, Package, Key]() { if (RevertPackage(Package.Get())) Dismiss(Key); });
        }));
    Notices.Add(Key, FSlateNotificationManager::Get().AddNotification(Info));
}

void FGitSoftLockEditorModule::Dismiss(const FString& Key)
{
    if (auto* Entry = Notices.Find(Key)) if (auto Item = Entry->Pin()) Item->ExpireAndFadeout();
    Notices.Remove(Key);
}

void FGitSoftLockEditorModule::AcknowledgeTesting(const FString& Path) { Testing.Add(Path); }

bool FGitSoftLockEditorModule::RevertPackage(UPackage* Package)
{
    FString Path;
    if (!PackagePath(Package, Path)) return false;
    FString Why;
    if (!IFileManager::Get().FileExists(*(Root / Path)))
        Why = TEXT("This package has no local disk version to reload. Close its editor and discard it manually.");
    else if (GEditor && GEditor->PlayWorld)
        Why = TEXT("Stop Play In Editor before reverting packages.");
    else if (Package->ContainsMap())
    {
        // Reloading the world destroys its actors. Never discard a second dirty package implicitly.
        for (TObjectIterator<UPackage> It; It; ++It)
            if (*It != Package && It->IsDirty() && !It->HasAnyFlags(RF_Transient) && !It->HasAnyPackageFlags(PKG_PlayInEditor))
            { Why = TEXT("Map reload is blocked while other packages have unsaved changes. Save or discard those first."); break; }
    }
    if (!Why.IsEmpty())
    {
        UE_LOG(LogGitSoftLock, Warning, TEXT("%s"), *Why);
        if (!IsRunningCommandlet())
        {
            FNotificationInfo Info(FText::FromString(Why)); Info.ExpireDuration = 12;
            FSlateNotificationManager::Get().AddNotification(Info);
        }
        return false;
    }
    TGuardValue<bool> Guard(bReverting, true);
    const TWeakObjectPtr<UPackage> Weak(Package);
    const bool bWasDirty = Package->IsDirty();
    FText Error;
    const bool bOK = UPackageTools::ReloadPackages({Package}, Error, EReloadPackagesInteractionMode::AssumePositive);
    if (bOK) { Testing.Remove(Path); Baselines.Remove(Path); OriginalPaths.Remove(Weak); Dismiss(TEXT("edit:") + Path); Dismiss(TEXT("save:") + Path); }
    else
    {
        if (Weak.IsValid() && bWasDirty) Weak->SetDirtyFlag(true);
        UE_LOG(LogGitSoftLock, Warning, TEXT("Reload failed: %s"), *Error.ToString());
    }
    return bOK;
}

bool FGitSoftLockEditorModule::Tick(float)
{
    auto Work = MoveTemp(Deferred); Deferred.Reset();
    for (auto& Action : Work) Action();
    if (!bEnabled) return true;
    Requests.RemoveAll([](const auto& Request) { const auto S = Request->GetStatus(); return S == EHttpRequestStatus::Succeeded || S == EHttpRequestStatus::Failed; });
    const double Now = FPlatformTime::Seconds();
    if (Now >= NextPoll) { NextPoll = Now + 2.0; Poll(); }
    FinishReleaseCheck();
    if (bAutoRelease && Now >= NextRelease && !ReleaseTask.IsValid()) { NextRelease = Now + 20; BeginReleaseCheck(); }
    // Recovery and edits that do not call UObject::Modify are picked up from dirty packages.
    if (Now >= NextDirtyScan)
    {
        NextDirtyScan = Now + 5;
        for (TObjectIterator<UPackage> It; It; ++It) if (It->IsDirty()) TrackModified(*It);
    }
    return true;
}

void FGitSoftLockEditorModule::Poll()
{
    if (!Connection.IsValid() || bPollPending) return;
    auto Request = Connection.Request(TEXT("snapshot"), Connection.Body());
    bPollPending = true;
    Request->OnProcessRequestComplete().BindLambda([this](FHttpRequestPtr, FHttpResponsePtr Response, bool) {
        bPollPending = false;
        auto Reply = FGitSoftLockConnection::Parse(Response);
        bConnected = GitSoftLockGranted(Reply);
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (!bConnected || !Reply->TryGetArrayField(TEXT("locks"), Rows)) { bConnected = false; return; }
        NetworkFailureUntil = 0;
        Locks.Reset();
        for (const auto& Value : *Rows)
        {
            auto Lock = Value->AsObject(); FString Path;
            if (Lock && Lock->TryGetStringField(TEXT("path"), Path)) Locks.Add(Path.ToLower(), Lock);
        }
    });
    Requests.Add(Request);
    if (!Request->ProcessRequest()) { bPollPending = false; bConnected = false; }
}

void FGitSoftLockEditorModule::BeginReleaseCheck()
{
    if (!bConnected) return;
    TArray<FGitSoftLockReleaseCandidate> Candidates;
    for (const auto& Pair : Locks)
    {
        bool bMine = false;
        if (!Pair.Value->TryGetBoolField(TEXT("mine"), bMine) || !bMine) continue;
        bool bDirty = false;
        for (const auto& P : Packages)
            if (P.Key.Equals(Pair.Key, ESearchCase::IgnoreCase) && P.Value.IsValid() && P.Value->IsDirty()) bDirty = true;
        if (bDirty || PendingClaims.Contains(Pair.Key)) continue;
        Candidates.Add({Pair.Key, Pair.Value->GetStringField(TEXT("generation")), FString()});
        if (Candidates.Num() >= 8) break;
    }
    if (Candidates.IsEmpty()) return;
    ReleaseTask = Async(EAsyncExecution::ThreadPool, [Candidates, Git = GitExecutable, RepoRoot = Root]() {
        TArray<FGitSoftLockReleaseCandidate> Eligible;
        const FString Prefix = TEXT("-C ") + QuoteGit(RepoRoot) + TEXT(" ");
        auto Run = [&Git, &Prefix](const FString& Args, FString& Output) {
            int32 Code = -1; FString Errors;
            FPlatformProcess::ExecProcess(*Git, *(Prefix + Args), &Code, &Output, &Errors);
            return Code == 0;
        };
        FString Out;
        if (!Run(TEXT("rev-parse --verify @{upstream}"), Out)) return Eligible;
        for (auto Candidate : Candidates)
        {
            const FString Quoted = QuoteGit(Candidate.Path);
            if (Quoted.IsEmpty()) continue;
            if (!Run(TEXT("status --porcelain=v1 --untracked-files=all -- ") + Quoted, Out) || !Out.TrimStartAndEnd().IsEmpty()) continue;
            if (!Run(TEXT("diff --quiet @{upstream} -- ") + Quoted, Out)) continue;
            Candidate.Fingerprint = GitSoftLockFingerprint(RepoRoot / Candidate.Path);
            if (!Candidate.Fingerprint.IsEmpty()) Eligible.Add(Candidate);
        }
        return Eligible;
    });
}

void FGitSoftLockEditorModule::FinishReleaseCheck()
{
    if (!ReleaseTask.IsValid() || !ReleaseTask.IsReady()) return;
    const auto Candidates = ReleaseTask.Get(); ReleaseTask.Reset();
    for (const auto& Candidate : Candidates)
    {
        bool bDirty = false;
        // Scan live objects too: a package may have become dirty while Git was queried.
        for (TObjectIterator<UPackage> It; It; ++It)
        {
            FString Path;
            if (It->IsDirty() && PackagePath(*It, Path) && Path.Equals(Candidate.Path, ESearchCase::IgnoreCase)) { bDirty = true; break; }
        }
        const auto* Lock = Locks.Find(Candidate.Path);
        if (bDirty || !Lock || (*Lock)->GetStringField(TEXT("generation")) != Candidate.Generation) continue;
        // File changes after Git's check are a reason to retain ownership.
        if (GitSoftLockFingerprint(Root / Candidate.Path) != Candidate.Fingerprint) continue;
        auto Body = Connection.Body();
        Body->SetStringField(TEXT("path"), Candidate.Path);
        Body->SetStringField(TEXT("generation"), Candidate.Generation);
        Body->SetStringField(TEXT("fingerprint"), Candidate.Fingerprint);
        if (GitSoftLockGranted(Connection.Send(TEXT("release"), Body)))
        {
            Locks.Remove(Candidate.Path);
            UE_LOG(LogGitSoftLock, Display, TEXT("Released clean published asset: %s"), *Candidate.Path);
        }
    }
}

FString FGitSoftLockEditorModule::GetStatusText() const
{
    FString Text = !bEnabled ? TEXT("Protection OFF - configure Project Settings > Plugins > Git Soft Lock, then restart.") :
        (bConnected ? TEXT("Connected - protected saves require server ownership.") : TEXT("Disconnected or not configured - protected saves are blocked."));
    Text += TEXT("\nRepository: ") + Connection.Repository + TEXT("\nServer: ") + Connection.Url + TEXT("\n\n");
    TArray<FString> Keys; Locks.GetKeys(Keys); Keys.Sort();
    for (const FString& Key : Keys)
    {
        const auto& Lock = Locks[Key]; bool bMine = false, bOnline = false;
        Lock->TryGetBoolField(TEXT("mine"), bMine); Lock->TryGetBoolField(TEXT("online"), bOnline);
        Text += Key + TEXT("\n    ") + Lock->GetStringField(TEXT("name")) + (bMine ? TEXT(" (this workspace)") : TEXT("")) +
            (bOnline ? TEXT(" - online\n\n") : TEXT(" - offline; lock retained\n\n"));
    }
    return Text;
}

TSharedRef<SDockTab> FGitSoftLockEditorModule::SpawnDashboard(const FSpawnTabArgs&)
{
    return SNew(SDockTab).TabRole(ETabRole::NomadTab)
        [SNew(SScrollBox) + SScrollBox::Slot().Padding(16)
            [SNew(STextBlock).Text_Lambda([this]() { return FText::FromString(GetStatusText()); }).AutoWrapText(true)]];
}

void FGitSoftLockEditorModule::RegisterMenus()
{
    FToolMenuOwnerScoped Owner(this);
    UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
    Menu->FindOrAddSection(TEXT("SourceControl")).AddMenuEntry(TEXT("GitSoftLock"), FText::FromString(TEXT("Git Soft Lock")),
        FText::FromString(TEXT("View shared asset locks and connection status")), FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([]() { FGlobalTabmanager::Get()->TryInvokeTab(GitSoftLockTab); })));
}

IMPLEMENT_MODULE(FGitSoftLockEditorModule, GitSoftLockEditor)
