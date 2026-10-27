#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "GitSoftLockClient.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectGlobals.h"
#include "Async/Future.h"

class SNotificationItem;
class SDockTab;
class FSpawnTabArgs;

struct FGitSoftLockReleaseCandidate
{
    FString Path, Generation, Fingerprint;
};

class FGitSoftLockEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    virtual bool SupportsDynamicReloading() override { return false; }

    bool CanSave(UPackage* Package, const FString& Filename, FOutputDevice* Error);
    bool RevertPackage(UPackage* Package);
    void AcknowledgeTesting(const FString& Path);
    void SetConnectionForTests(const FGitSoftLockConnection& Value) { Connection = Value; bEnabled = true; Locks.Reset(); NetworkFailureUntil = 0; }
    FCoreUObjectDelegates::FIsPackageOKToSaveDelegate SwapPreviousSaveForTests(FCoreUObjectDelegates::FIsPackageOKToSaveDelegate Value)
    { auto Old = PreviousSave; PreviousSave = Value; return Old; }
    void TrackModified(UPackage* Package);
    FString GetStatusText() const;

private:
    bool Tick(float Delta);
    bool PackagePath(UPackage* Package, FString& OutPath) const;
    FString Baseline(UPackage* Package, const FString& Path);
    void RememberLoaded(const FEndLoadPackageContext& Context);
    void Claim(UPackage* Package, const FString& Path);
    void Poll();
    void QueueWarning(UPackage* Package, const FString& Path, const TSharedPtr<FJsonObject>& Reply, bool bSave);
    void ShowWarning(TWeakObjectPtr<UPackage> Package, FString Path, FString Reason, bool bSave);
    void Dismiss(const FString& Key);
    void BeginReleaseCheck();
    void FinishReleaseCheck();
    void RegisterMenus();
    TSharedRef<SDockTab> SpawnDashboard(const FSpawnTabArgs& Args);

    FGitSoftLockConnection Connection;
    FString Root, GitExecutable;
    bool bEnabled = false, bReverting = false, bStopping = false, bConnected = false;
    bool bAutoRelease = true;
    bool bPollPending = false;
    double NextDirtyScan = 0;
    double NextPoll = 0, NextRelease = 0, NetworkFailureUntil = 0;
    FCoreUObjectDelegates::FIsPackageOKToSaveDelegate PreviousSave;
    FDelegateHandle SaveHandle;
    FDelegateHandle ModifiedHandle, DirtyHandle, SavedHandle, DeleteHandle, DestructiveHandle;
    FTSTicker::FDelegateHandle Ticker;
    TMap<FString, FDateTime> LoadedTimes;
    TMap<FString, FString> Baselines;
    TMap<FString, TWeakObjectPtr<UPackage>> Packages;
    TMap<TWeakObjectPtr<UPackage>, FString> OriginalPaths;
    TMap<FString, TSharedPtr<FJsonObject>> Locks;
    TSet<FString> PendingClaims, Testing;
    TMap<FString, TWeakPtr<SNotificationItem>> Notices;
    TArray<TFunction<void()>> Deferred;
    TArray<TSharedRef<IHttpRequest, ESPMode::ThreadSafe>> Requests;
    TFuture<TArray<FGitSoftLockReleaseCandidate>> ReleaseTask;
    TUniquePtr<FArchive> WorkspaceGuard;
};
