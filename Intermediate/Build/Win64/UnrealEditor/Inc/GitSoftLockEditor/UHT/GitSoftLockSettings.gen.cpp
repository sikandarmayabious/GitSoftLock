// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

#include "UObject/GeneratedCppIncludes.h"
#include "GitSoftLockSettings.h"

PRAGMA_DISABLE_DEPRECATION_WARNINGS
static_assert(!UE_WITH_CONSTINIT_UOBJECT, "This generated code can only be compiled with !UE_WITH_CONSTINIT_UOBJECT");
void EmptyLinkFunctionForGeneratedCodeGitSoftLockSettings() {}

// ********** Begin Cross Module References ********************************************************
DEVELOPERSETTINGS_API UClass* Z_Construct_UClass_UDeveloperSettings(ETypeConstructPhase);
// ********** End Cross Module References **********************************************************

// ********** Begin Same Module References *********************************************************
UPackage* Z_Construct_UPackage__Script_GitSoftLockEditor(ETypeConstructPhase);
GITSOFTLOCKEDITOR_API UClass* Z_Construct_UClass_UGitSoftLockSettings(ETypeConstructPhase);
GITSOFTLOCKEDITOR_API UClass* Z_Construct_UClass_UGitSoftLockSettings(ETypeConstructPhase);
// ********** End Same Module References ***********************************************************
#define UHT_STRUCT_BASE(INIT) UE::CodeGen::ConstInit::TCompiledInObjectPtr<const FStructBaseChain>(UE::Private::AsStructBaseChain(INIT))

// ********** Begin Class UGitSoftLockSettings *****************************************************
#ifdef UHT_STATICS
#error UHT_STATICS already defined
#endif
#define UHT_STATICS Z_Construct_UClass_UGitSoftLockSettings_Statics
struct UHT_STATICS
{
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Type_MetaData[] = {
		{ "DisplayName", "Git Soft Lock" },
		{ "IncludePath", "GitSoftLockSettings.h" },
		{ "ModuleRelativePath", "Public/GitSoftLockSettings.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_bEnabled_MetaData[] = {
		{ "Category", "Connection" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "// Configure the shared URL/repository first, then enable for the whole team. Restart required.\n" },
#endif
		{ "ConfigRestartRequired", "TRUE" },
		{ "ModuleRelativePath", "Public/GitSoftLockSettings.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Configure the shared URL/repository first, then enable for the whole team. Restart required." },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_ServerUrl_MetaData[] = {
		{ "Category", "Connection" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "// HTTPS required except loopback development. Do not put credentials in this URL.\n" },
#endif
		{ "ConfigRestartRequired", "TRUE" },
		{ "ModuleRelativePath", "Public/GitSoftLockSettings.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "HTTPS required except loopback development. Do not put credentials in this URL." },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_RepositoryId_MetaData[] = {
		{ "Category", "Connection" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "// Identical on every clone and branch; authorized by the server administrator.\n" },
#endif
		{ "ConfigRestartRequired", "TRUE" },
		{ "ModuleRelativePath", "Public/GitSoftLockSettings.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Identical on every clone and branch; authorized by the server administrator." },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_RequestTimeoutSeconds_MetaData[] = {
		{ "Category", "Connection" },
		{ "ClampMax", "10.0" },
		{ "ClampMin", "1.0" },
		{ "ConfigRestartRequired", "TRUE" },
		{ "ModuleRelativePath", "Public/GitSoftLockSettings.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_GitExecutable_MetaData[] = {
		{ "Category", "Git" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "// Git executable on PATH or an absolute filename. Used only for read-only release checks.\n" },
#endif
		{ "ConfigRestartRequired", "TRUE" },
		{ "ModuleRelativePath", "Public/GitSoftLockSettings.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Git executable on PATH or an absolute filename. Used only for read-only release checks." },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_bAutoReleasePublishedFiles_MetaData[] = {
		{ "Category", "Git" },
		{ "ConfigRestartRequired", "TRUE" },
		{ "ModuleRelativePath", "Public/GitSoftLockSettings.h" },
	};
#endif // WITH_METADATA

// ********** Begin Class UGitSoftLockSettings constinit property declarations *********************
	static void NewProp_bEnabled_SetBit(void* Obj)
	{
		((UGitSoftLockSettings*)Obj)->bEnabled = 1;
	}
	static const UECodeGen_Private::FBoolPropertyParams NewProp_bEnabled;
	static const UECodeGen_Private::FStrPropertyParams NewProp_ServerUrl;
	static const UECodeGen_Private::FStrPropertyParams NewProp_RepositoryId;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_RequestTimeoutSeconds;
	static const UECodeGen_Private::FStrPropertyParams NewProp_GitExecutable;
	static void NewProp_bAutoReleasePublishedFiles_SetBit(void* Obj)
	{
		((UGitSoftLockSettings*)Obj)->bAutoReleasePublishedFiles = 1;
	}
	static const UECodeGen_Private::FBoolPropertyParams NewProp_bAutoReleasePublishedFiles;
	static const UECodeGen_Private::FPropertyParamsBase* const PropPointers[];
// ********** End Class UGitSoftLockSettings constinit property declarations ***********************
	static FTypeConstructFunc* DependentSingletons[];
	static constexpr FCppClassTypeInfoStatic StaticCppClassTypeInfo = {
		TCppClassTypeTraits<UGitSoftLockSettings>::IsAbstract,
	};
	static const UECodeGen_Private::FClassParams ClassParams;
}; // struct UHT_STATICS

// ********** Begin Class UGitSoftLockSettings Property Definitions ********************************
const UECodeGen_Private::FBoolPropertyParams UHT_STATICS::NewProp_bEnabled = { "bEnabled", nullptr, (EPropertyFlags)0x0010000000004001, UECodeGen_Private::EPropertyGenFlags::Bool | UECodeGen_Private::EPropertyGenFlags::NativeBool, nullptr, nullptr, 1, sizeof(bool), sizeof(UGitSoftLockSettings), &UHT_STATICS::NewProp_bEnabled_SetBit, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_bEnabled_MetaData), NewProp_bEnabled_MetaData) };
const UECodeGen_Private::FStrPropertyParams UHT_STATICS::NewProp_ServerUrl = { "ServerUrl", nullptr, (EPropertyFlags)0x0010000000004001, UECodeGen_Private::EPropertyGenFlags::Str, nullptr, nullptr, 1, STRUCT_OFFSET(UGitSoftLockSettings, ServerUrl), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_ServerUrl_MetaData), NewProp_ServerUrl_MetaData) };
const UECodeGen_Private::FStrPropertyParams UHT_STATICS::NewProp_RepositoryId = { "RepositoryId", nullptr, (EPropertyFlags)0x0010000000004001, UECodeGen_Private::EPropertyGenFlags::Str, nullptr, nullptr, 1, STRUCT_OFFSET(UGitSoftLockSettings, RepositoryId), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_RepositoryId_MetaData), NewProp_RepositoryId_MetaData) };
const UECodeGen_Private::FFloatPropertyParams UHT_STATICS::NewProp_RequestTimeoutSeconds = { "RequestTimeoutSeconds", nullptr, (EPropertyFlags)0x0010000000004001, UECodeGen_Private::EPropertyGenFlags::Float, nullptr, nullptr, 1, STRUCT_OFFSET(UGitSoftLockSettings, RequestTimeoutSeconds), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_RequestTimeoutSeconds_MetaData), NewProp_RequestTimeoutSeconds_MetaData) };
const UECodeGen_Private::FStrPropertyParams UHT_STATICS::NewProp_GitExecutable = { "GitExecutable", nullptr, (EPropertyFlags)0x0010000000004001, UECodeGen_Private::EPropertyGenFlags::Str, nullptr, nullptr, 1, STRUCT_OFFSET(UGitSoftLockSettings, GitExecutable), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_GitExecutable_MetaData), NewProp_GitExecutable_MetaData) };
const UECodeGen_Private::FBoolPropertyParams UHT_STATICS::NewProp_bAutoReleasePublishedFiles = { "bAutoReleasePublishedFiles", nullptr, (EPropertyFlags)0x0010000000004001, UECodeGen_Private::EPropertyGenFlags::Bool | UECodeGen_Private::EPropertyGenFlags::NativeBool, nullptr, nullptr, 1, sizeof(bool), sizeof(UGitSoftLockSettings), &UHT_STATICS::NewProp_bAutoReleasePublishedFiles_SetBit, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_bAutoReleasePublishedFiles_MetaData), NewProp_bAutoReleasePublishedFiles_MetaData) };
const UECodeGen_Private::FPropertyParamsBase* const UHT_STATICS::PropPointers[] = {
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_bEnabled,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_ServerUrl,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_RepositoryId,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_RequestTimeoutSeconds,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_GitExecutable,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_bAutoReleasePublishedFiles,
};
static_assert(UE_ARRAY_COUNT(UHT_STATICS::PropPointers) < 2048);
// ********** End Class UGitSoftLockSettings Property Definitions **********************************
FTypeConstructFunc* UHT_STATICS::DependentSingletons[] = {
	(FTypeConstructFunc*)Z_Construct_UClass_UDeveloperSettings,
	(FTypeConstructFunc*)Z_Construct_UPackage__Script_GitSoftLockEditor,
};
static_assert(UE_ARRAY_COUNT(UHT_STATICS::DependentSingletons) < 16);
const UECodeGen_Private::FClassParams UHT_STATICS::ClassParams = {
	&Z_Construct_UClass_UGitSoftLockSettings,
	"Editor",
	&StaticCppClassTypeInfo,
	DependentSingletons,
	nullptr,
	UHT_STATICS::PropPointers,
	nullptr,
	UE_ARRAY_COUNT(DependentSingletons),
	0,
	UE_ARRAY_COUNT(UHT_STATICS::PropPointers),
	0,
	0x001000A6u,
	METADATA_PARAMS(UE_ARRAY_COUNT(UHT_STATICS::Type_MetaData), UHT_STATICS::Type_MetaData)
};
FClassRegistrationInfo Z_Registration_Info_UClass_UGitSoftLockSettings;
UClass* Z_Construct_UClass_UGitSoftLockSettings(ETypeConstructPhase Phase)
{
	if (Phase == ETypeConstructPhase::Inner)
	{
		using TClass = UGitSoftLockSettings;
		if (!Z_Registration_Info_UClass_UGitSoftLockSettings.InnerSingleton)
		{
			GetPrivateStaticClassBody(
				TClass::StaticPackage(),
				TEXT("GitSoftLockSettings"),
				Z_Registration_Info_UClass_UGitSoftLockSettings.InnerSingleton,
				nullptr,
				DataSizeOf<TClass>(),
				alignof(TClass),
				TClass::StaticClassFlags,
				TClass::StaticClassCastFlags(),
				TClass::StaticConfigName(),
				(UClass::ClassConstructorType)InternalConstructor<TClass>,
				(UClass::ClassVTableHelperCtorCallerType)InternalVTableHelperCtorCaller<TClass>,
				UOBJECT_CPPCLASS_STATICFUNCTIONS_FORCLASS(TClass),
				&TClass::Super::StaticClass,
				&TClass::WithinClass::StaticClass
			);
		}
		return Z_Registration_Info_UClass_UGitSoftLockSettings.InnerSingleton;
	}
	if (!Z_Registration_Info_UClass_UGitSoftLockSettings.OuterSingleton)
	{
		UECodeGen_Private::ConstructUClass(Z_Registration_Info_UClass_UGitSoftLockSettings.OuterSingleton, UHT_STATICS::ClassParams);
	}
	return Z_Registration_Info_UClass_UGitSoftLockSettings.OuterSingleton;
}
#undef UHT_STATICS
DEFINE_VTABLE_PTR_HELPER_CTOR_NS(, UGitSoftLockSettings);
UGitSoftLockSettings::~UGitSoftLockSettings() {}
// ********** End Class UGitSoftLockSettings *******************************************************

// ********** Begin Registration *******************************************************************
#ifdef UHT_STATICS
#error UHT_STATICS already defined
#endif
#define UHT_STATICS Z_CompiledInDeferFile_FID_RAYMOND_SION_UE_Plugins_GitSoftLock_Source_GitSoftLockEditor_Public_GitSoftLockSettings_h__Script_GitSoftLockEditor_Statics
struct UHT_STATICS
{
	static constexpr FClassRegisterCompiledInInfo ClassInfo[] = {
		{ Z_Construct_UClass_UGitSoftLockSettings, TEXT("UGitSoftLockSettings"), &Z_Registration_Info_UClass_UGitSoftLockSettings, CONSTRUCT_RELOAD_VERSION_INFO(FClassReloadVersionInfo, sizeof(UGitSoftLockSettings), 1754582831U) },
	};
}; // UHT_STATICS 
static FRegisterCompiledInInfo Z_CompiledInDeferFile_FID_RAYMOND_SION_UE_Plugins_GitSoftLock_Source_GitSoftLockEditor_Public_GitSoftLockSettings_h__Script_GitSoftLockEditor_ad12fbcd4c5a9b9b86f694492ab806413264ec7b{
	TEXT("/Script/GitSoftLockEditor"),
	UHT_STATICS::ClassInfo, UE_ARRAY_COUNT(UHT_STATICS::ClassInfo),
	nullptr, 0,
	nullptr, 0,
	nullptr, 0,
};
#undef UHT_STATICS
// ********** End Registration *********************************************************************
#undef UHT_STRUCT_BASE

PRAGMA_ENABLE_DEPRECATION_WARNINGS
