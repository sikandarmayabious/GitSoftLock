#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Interfaces/IHttpRequest.h"

struct FGitSoftLockConnection
{
    FString Url, Repository, Workspace, Token;
    float Timeout = 3.0f;
    bool IsValid() const;
    TSharedRef<FJsonObject> Body() const;
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request(const FString& Action, const TSharedRef<FJsonObject>& Body) const;
    TSharedPtr<FJsonObject> Send(const FString& Action, const TSharedRef<FJsonObject>& Body) const;
    static TSharedPtr<FJsonObject> Parse(const FHttpResponsePtr& Response);
};

bool GitSoftLockGranted(const TSharedPtr<FJsonObject>& Reply);
FString GitSoftLockFingerprint(const FString& Filename);
bool GitSoftLockRelativePath(const FString& Root, const FString& Filename, FString& OutPath);
