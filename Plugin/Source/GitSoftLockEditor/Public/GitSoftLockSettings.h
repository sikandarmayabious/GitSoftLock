#pragma once

#include "Engine/DeveloperSettings.h"
#include "GitSoftLockSettings.generated.h"

UCLASS(Config=Editor, DefaultConfig, meta=(DisplayName="Git Soft Lock"))
class GITSOFTLOCKEDITOR_API UGitSoftLockSettings : public UDeveloperSettings
{
    GENERATED_BODY()
public:
    UGitSoftLockSettings() { CategoryName = TEXT("Plugins"); }

    // Configure the shared URL/repository first, then enable for the whole team. Restart required.
    UPROPERTY(Config, EditAnywhere, Category="Connection", meta=(ConfigRestartRequired=true))
    bool bEnabled = false;

    // HTTPS required except loopback development. Do not put credentials in this URL.
    UPROPERTY(Config, EditAnywhere, Category="Connection", meta=(ConfigRestartRequired=true))
    FString ServerUrl;

    // Set for each consuming project; identical on its clones and branches and authorized by the server administrator.
    UPROPERTY(Config, EditAnywhere, Category="Connection", meta=(ConfigRestartRequired=true))
    FString RepositoryId;

    UPROPERTY(Config, EditAnywhere, Category="Connection", meta=(ClampMin="1.0", ClampMax="10.0", ConfigRestartRequired=true))
    float RequestTimeoutSeconds = 3.0f;

    // Git executable on PATH or an absolute filename. Used only for read-only release checks.
    UPROPERTY(Config, EditAnywhere, Category="Git", meta=(ConfigRestartRequired=true))
    FString GitExecutable = TEXT("git");

    UPROPERTY(Config, EditAnywhere, Category="Git", meta=(ConfigRestartRequired=true))
    bool bAutoReleasePublishedFiles = true;
};
