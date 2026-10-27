#include "GitSoftLockClient.h"
#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

bool FGitSoftLockConnection::IsValid() const
{
    return (Url.StartsWith(TEXT("https://")) || Url.StartsWith(TEXT("http://127.0.0.1:")) ||
        Url.StartsWith(TEXT("http://localhost:"))) && !Url.Contains(TEXT("@")) &&
        !Repository.IsEmpty() && !Workspace.IsEmpty() && !Token.IsEmpty();
}

TSharedRef<FJsonObject> FGitSoftLockConnection::Body() const
{
    auto Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("protocol"), 1);
    Result->SetStringField(TEXT("repo"), Repository);
    Result->SetStringField(TEXT("workspace"), Workspace);
    return Result;
}

TSharedRef<IHttpRequest, ESPMode::ThreadSafe> FGitSoftLockConnection::Request(const FString& Action, const TSharedRef<FJsonObject>& Data) const
{
    auto Result = FHttpModule::Get().CreateRequest();
    Result->SetURL(Url / TEXT("v1") / Action);
    Result->SetVerb(TEXT("POST"));
    Result->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Result->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + Token);
    Result->SetTimeout(Timeout);
    FString Encoded;
    FJsonSerializer::Serialize(Data, TJsonWriterFactory<>::Create(&Encoded));
    Result->SetContentAsString(Encoded);
    return Result;
}

TSharedPtr<FJsonObject> FGitSoftLockConnection::Parse(const FHttpResponsePtr& Response)
{
    if (!Response.IsValid() || Response->GetResponseCode() != 200) return nullptr;
    TSharedPtr<FJsonObject> Result;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()), Result)) return nullptr;
    double Protocol = 0;
    return Result && Result->TryGetNumberField(TEXT("protocol"), Protocol) && Protocol == 1 ? Result : nullptr;
}

TSharedPtr<FJsonObject> FGitSoftLockConnection::Send(const FString& Action, const TSharedRef<FJsonObject>& Data) const
{
    if (!IsValid()) return nullptr;
    auto Req = Request(Action, Data);
    // UE 5.8 supplies a blocking operation which completes on the HTTP thread.
    // This avoids waiting for a game-thread callback from inside SavePackage.
    Req->ProcessRequestUntilComplete();
    return Parse(Req->GetResponse());
}

bool GitSoftLockGranted(const TSharedPtr<FJsonObject>& Reply)
{
    bool bOK = false;
    return Reply && Reply->TryGetBoolField(TEXT("ok"), bOK) && bOK;
}

FString GitSoftLockFingerprint(const FString& Filename)
{
    if (!IFileManager::Get().FileExists(*Filename)) return TEXT("missing");
    const FMD5Hash Hash = FMD5Hash::HashFile(*Filename);
    return Hash.IsValid() ? LexToString(Hash).ToLower() : FString();
}

bool GitSoftLockRelativePath(const FString& Root, const FString& Filename, FString& OutPath)
{
    FString Full = FPaths::ConvertRelativePathToFull(Filename);
    FPaths::NormalizeFilename(Full);
    FPaths::CollapseRelativeDirectories(Full);
    FString Prefix = FPaths::ConvertRelativePathToFull(Root);
    FPaths::NormalizeDirectoryName(Prefix);
    Prefix += TEXT("/");
    if (!Full.StartsWith(Prefix, ESearchCase::IgnoreCase)) return false;
    OutPath = Full.Mid(Prefix.Len());
    return OutPath.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase) || OutPath.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase);
}
