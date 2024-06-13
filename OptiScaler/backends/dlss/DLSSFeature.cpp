#include "../../pch.h"
#include "../../Config.h"
#include "../../Util.h"

#include "DLSSFeature.h"

#include "nvapi.h"

#include <filesystem>
#include "../../detours/detours.h"

#pragma region spoofing hooks for 16xx

// NvAPI_GPU_GetArchInfo hooking based on Nukem's spoofing code here
// https://github.com/Nukem9/dlssg-to-fsr3/blob/89ddc8c1cce4593fb420e633a06605c3c4b9c3cf/source/wrapper_generic/nvapi.cpp#L50

enum class NV_INTERFACE : uint32_t
{
	GPU_GetArchInfo = 0xD8265D24,
	D3D12_SetRawScgPriority = 0x5DB3048A,
};

typedef void* (__stdcall* PFN_NvApi_QueryInterface)(NV_INTERFACE InterfaceId);
typedef NVSDK_NGX_Result(*PFN_NVSDK_NGX_GetFeatureRequirements)(IDXGIAdapter* Adapter, const NVSDK_NGX_FeatureDiscoveryInfo* FeatureDiscoveryInfo, NVSDK_NGX_FeatureRequirement* OutSupported);

using PfnNvAPI_GPU_GetArchInfo = uint32_t(__stdcall*)(void* GPUHandle, NV_GPU_ARCH_INFO* ArchInfo);

PFN_NvApi_QueryInterface OriginalNvAPI_QueryInterface = nullptr;
PfnNvAPI_GPU_GetArchInfo OriginalNvAPI_GPU_GetArchInfo = nullptr;
PFN_NVSDK_NGX_GetFeatureRequirements Original_Dx11_GetFeatureRequirements = nullptr;
PFN_NVSDK_NGX_GetFeatureRequirements Original_Dx12_GetFeatureRequirements = nullptr;

uint32_t __stdcall HookedNvAPI_GPU_GetArchInfo(void* GPUHandle, NV_GPU_ARCH_INFO* ArchInfo)
{
	if (OriginalNvAPI_GPU_GetArchInfo)
	{
		const auto status = OriginalNvAPI_GPU_GetArchInfo(GPUHandle, ArchInfo);

		if (status == 0 && ArchInfo)
		{
			spdlog::debug("DLSSFeature HookedNvAPI_GPU_GetArchInfo From api arch: {0:X} impl: {1:X} rev: {2:X}!", ArchInfo->architecture, ArchInfo->implementation, ArchInfo->revision);

			// for 16xx cards
			if (ArchInfo->architecture == NV_GPU_ARCHITECTURE_TU100 && ArchInfo->implementation > NV_GPU_ARCH_IMPLEMENTATION_TU106)
			{
				ArchInfo->implementation = NV_GPU_ARCH_IMPLEMENTATION_TU106;
				ArchInfo->implementation_id = NV_GPU_ARCH_IMPLEMENTATION_TU106;

				spdlog::info("DLSSFeature HookedNvAPI_GPU_GetArchInfo Spoofed arch: {0:X} impl: {1:X} rev: {2:X}!", ArchInfo->architecture, ArchInfo->implementation, ArchInfo->revision);
			}
		}

		return status;
	}

	return 0xFFFFFFFF;
}

void* __stdcall HookedNvAPI_QueryInterface(NV_INTERFACE InterfaceId)
{
	const auto result = OriginalNvAPI_QueryInterface(InterfaceId);

	if (result)
	{
		if (InterfaceId == NV_INTERFACE::GPU_GetArchInfo)
		{
			OriginalNvAPI_GPU_GetArchInfo = static_cast<PfnNvAPI_GPU_GetArchInfo>(result);
			return &HookedNvAPI_GPU_GetArchInfo;
		}
	}

	return result;
}

NVSDK_NGX_Result __stdcall Hooked_Dx12_GetFeatureRequirements(IDXGIAdapter* Adapter, const NVSDK_NGX_FeatureDiscoveryInfo* FeatureDiscoveryInfo, NVSDK_NGX_FeatureRequirement* OutSupported)
{
	spdlog::debug("Hooked_Dx12_GetFeatureRequirements!");

	auto result = Original_Dx12_GetFeatureRequirements(Adapter, FeatureDiscoveryInfo, OutSupported);

	if (result == NVSDK_NGX_Result_Success && FeatureDiscoveryInfo->FeatureID == NVSDK_NGX_Feature_SuperSampling)
	{
		spdlog::info("Hooked_Dx12_GetFeatureRequirements Spoofing support!");
		OutSupported->FeatureSupported = NVSDK_NGX_FeatureSupportResult_Supported;
		OutSupported->MinHWArchitecture = 0;
		strcpy_s(OutSupported->MinOSVersion, "10.0.10240.16384");
	}

	return result;
}

NVSDK_NGX_Result __stdcall Hooked_Dx11_GetFeatureRequirements(IDXGIAdapter* Adapter, const NVSDK_NGX_FeatureDiscoveryInfo* FeatureDiscoveryInfo, NVSDK_NGX_FeatureRequirement* OutSupported)
{
	spdlog::debug("Hooked_Dx11_GetFeatureRequirements!");

	auto result = Original_Dx11_GetFeatureRequirements(Adapter, FeatureDiscoveryInfo, OutSupported);

	if (result == NVSDK_NGX_Result_Success && FeatureDiscoveryInfo->FeatureID == NVSDK_NGX_Feature_SuperSampling)
	{
		spdlog::info("Hooked_Dx11_GetFeatureRequirements Spoofing support!");
		OutSupported->FeatureSupported = NVSDK_NGX_FeatureSupportResult_Supported;
		OutSupported->MinHWArchitecture = 0;
		strcpy_s(OutSupported->MinOSVersion, "10.0.10240.16384");
	}

	return result;
}

void HookNvApi()
{
	if (OriginalNvAPI_QueryInterface != nullptr)
		return;

	spdlog::debug("DLSSFeature Trying to hook NvApi");
	OriginalNvAPI_QueryInterface = (PFN_NvApi_QueryInterface)DetourFindFunction("nvapi64.dll", "nvapi_QueryInterface");
	spdlog::debug("DLSSFeature OriginalNvAPI_QueryInterface = {0:X}", (unsigned long long)OriginalNvAPI_QueryInterface);

	if (OriginalNvAPI_QueryInterface != nullptr)
	{
		spdlog::info("DLSSFeature NvAPI_QueryInterface found, hooking!");

		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(&(PVOID&)OriginalNvAPI_QueryInterface, HookedNvAPI_QueryInterface);
		DetourTransactionCommit();
	}
}

void HookNgxApi(HMODULE nvngx)
{
	if (Original_Dx11_GetFeatureRequirements != nullptr || Original_Dx12_GetFeatureRequirements != nullptr)
		return;

	spdlog::debug("DLSSFeature Trying to hook NgxApi");

	Original_Dx11_GetFeatureRequirements = (PFN_NVSDK_NGX_GetFeatureRequirements)GetProcAddress(nvngx, "NVSDK_NGX_D3D11_GetFeatureRequirements");
	Original_Dx12_GetFeatureRequirements = (PFN_NVSDK_NGX_GetFeatureRequirements)GetProcAddress(nvngx, "NVSDK_NGX_D3D12_GetFeatureRequirements");

	if (Original_Dx11_GetFeatureRequirements != nullptr || Original_Dx12_GetFeatureRequirements != nullptr)
	{
		spdlog::info("DLSSFeature NVSDK_NGX_D3D1X_GetFeatureRequirements found, hooking!");

		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());

		if (Original_Dx11_GetFeatureRequirements != nullptr)
			DetourAttach(&(PVOID&)Original_Dx11_GetFeatureRequirements, Hooked_Dx11_GetFeatureRequirements);

		if (Original_Dx12_GetFeatureRequirements != nullptr)
			DetourAttach(&(PVOID&)Original_Dx12_GetFeatureRequirements, Hooked_Dx12_GetFeatureRequirements);

		DetourTransactionCommit();
	}
}

void UnhookApis()
{
	if (OriginalNvAPI_QueryInterface != nullptr ||
		Original_Dx11_GetFeatureRequirements != nullptr || Original_Dx12_GetFeatureRequirements != nullptr)
	{
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());

		if (OriginalNvAPI_QueryInterface != nullptr)
			DetourDetach(&(PVOID&)OriginalNvAPI_QueryInterface, HookedNvAPI_QueryInterface);

		if (Original_Dx11_GetFeatureRequirements != nullptr)
			DetourDetach(&(PVOID&)Original_Dx11_GetFeatureRequirements, Hooked_Dx11_GetFeatureRequirements);

		if (Original_Dx12_GetFeatureRequirements != nullptr)
			DetourDetach(&(PVOID&)Original_Dx12_GetFeatureRequirements, Hooked_Dx12_GetFeatureRequirements);

		DetourTransactionCommit();
	}
}

#pragma endregion

void ProcessDx12Resources(const NVSDK_NGX_Parameter* InParameters, NVSDK_NGX_Parameter* OutParameters)
{
	ID3D12Resource* d3d12Resource;

	if (InParameters->Get(NVSDK_NGX_Parameter_Color, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_Color, d3d12Resource);
	else
		spdlog::error("DLSSFeature::ProcessDx12Resources no color input!");

	if (InParameters->Get(NVSDK_NGX_Parameter_Output, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_Output, d3d12Resource);
	else
		spdlog::error("DLSSFeature::ProcessDx12Resources no color output!");

	if (InParameters->Get(NVSDK_NGX_Parameter_Depth, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_Depth, d3d12Resource);
	else
		spdlog::warn("DLSSFeature::ProcessDx12Resources no depth input!");

	if (InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_MotionVectors, d3d12Resource);
	else
		spdlog::error("DLSSFeature::ProcessDx12Resources no motion input!");

	if (InParameters->Get(NVSDK_NGX_Parameter_TransparencyMask, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_TransparencyMask, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_ExposureTexture, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_ExposureTexture, d3d12Resource);
	else
		spdlog::debug("DLSSFeature::ProcessDx12Resources no exposure input!");

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, d3d12Resource);
	else
		spdlog::debug("DLSSFeature::ProcessDx12Resources no mask input!");

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Albedo, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Albedo, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Roughness, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Roughness, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Metallic, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Metallic, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Specular, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Specular, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Subsurface, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Subsurface, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Normals, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Normals, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_ShadingModelId, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_ShadingModelId, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_MaterialId, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_MaterialId, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_8, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_8, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_9, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_9, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_10, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_10, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_11, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_11, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_12, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_12, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_13, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_13, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_14, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_14, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_15, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_15, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_MotionVectors3D, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_MotionVectors3D, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_IsParticleMask, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_IsParticleMask, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_AnimatedTextureMask, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_AnimatedTextureMask, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_DepthHighRes, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_DepthHighRes, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_Position_ViewSpace, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_Position_ViewSpace, d3d12Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_MotionVectorsReflection, &d3d12Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_MotionVectorsReflection, d3d12Resource);
}

void ProcessDx11Resources(const NVSDK_NGX_Parameter* InParameters, NVSDK_NGX_Parameter* OutParameters)
{
	ID3D11Resource* d3d11Resource;

	if (InParameters->Get(NVSDK_NGX_Parameter_Color, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_Color, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_Output, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_Output, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_Depth, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_Depth, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_MotionVectors, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_TransparencyMask, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_TransparencyMask, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_ExposureTexture, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_ExposureTexture, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Albedo, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Albedo, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Roughness, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Roughness, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Metallic, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Metallic, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Specular, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Specular, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Subsurface, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Subsurface, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Normals, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Normals, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_ShadingModelId, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_ShadingModelId, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_MaterialId, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_MaterialId, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_8, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_8, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_9, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_9, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_10, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_10, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_11, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_11, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_12, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_12, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_13, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_13, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_14, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_14, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_15, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_15, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_MotionVectors3D, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_MotionVectors3D, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_IsParticleMask, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_IsParticleMask, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_AnimatedTextureMask, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_AnimatedTextureMask, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_DepthHighRes, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_DepthHighRes, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_Position_ViewSpace, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_Position_ViewSpace, d3d11Resource);

	if (InParameters->Get(NVSDK_NGX_Parameter_MotionVectorsReflection, &d3d11Resource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_MotionVectorsReflection, d3d11Resource);
}

void ProcessVulkanResources(const NVSDK_NGX_Parameter* InParameters, NVSDK_NGX_Parameter* OutParameters)
{
	void* vkResource;

	if (InParameters->Get(NVSDK_NGX_Parameter_Color, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_Color, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_Output, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_Output, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_Depth, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_Depth, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_MotionVectors, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_TransparencyMask, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_TransparencyMask, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_ExposureTexture, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_ExposureTexture, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Albedo, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Albedo, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Roughness, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Roughness, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Metallic, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Metallic, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Specular, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Specular, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Subsurface, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Subsurface, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Normals, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Normals, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_ShadingModelId, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_ShadingModelId, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_MaterialId, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_MaterialId, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_8, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_8, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_9, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_9, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_10, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_10, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_11, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_11, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_12, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_12, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_13, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_13, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_14, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_14, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Atrrib_15, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_GBuffer_Atrrib_15, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_MotionVectors3D, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_MotionVectors3D, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_IsParticleMask, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_IsParticleMask, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_AnimatedTextureMask, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_AnimatedTextureMask, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_DepthHighRes, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_DepthHighRes, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_Position_ViewSpace, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_Position_ViewSpace, vkResource);

	if (InParameters->Get(NVSDK_NGX_Parameter_MotionVectorsReflection, &vkResource) == NVSDK_NGX_Result_Success)
		OutParameters->Set(NVSDK_NGX_Parameter_MotionVectorsReflection, vkResource);
}

void DLSSFeature::ProcessEvaluateParams(const NVSDK_NGX_Parameter* InParameters)
{
	float floatValue;
	int intValue;
	unsigned int uintValue;

	if (Config::Instance()->Api == NVNGX_DX12)
		ProcessDx12Resources(InParameters, Parameters);
	else if (Config::Instance()->Api == NVNGX_DX11)
		ProcessDx11Resources(InParameters, Parameters);
	else
		ProcessVulkanResources(InParameters, Parameters);

	if (InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &floatValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, floatValue);
	else
		spdlog::error("DLSSFeature::ProcessEvaluateParams no jitter offset x!");

	if (InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &floatValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, floatValue);
	else
		spdlog::error("DLSSFeature::ProcessEvaluateParams no jitter offset y!");

	// override sharpness
	if (Config::Instance()->OverrideSharpness.value_or(false) && !(Config::Instance()->Api == NVNGX_DX12 && Config::Instance()->RcasEnabled.value_or(false)))
	{
		auto sharpness = Config::Instance()->Sharpness.value_or(0.3f);
		Parameters->Set(NVSDK_NGX_Parameter_Sharpness, sharpness);
	}
	// cas enabled
	else if (Config::Instance()->Api == NVNGX_DX12 && Config::Instance()->RcasEnabled.value_or(false))
		Parameters->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
	// dlss value
	else if (InParameters->Get(NVSDK_NGX_Parameter_Sharpness, &floatValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_Sharpness, floatValue);
	// fallback
	else
		Parameters->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);

	if (InParameters->Get(NVSDK_NGX_Parameter_Reset, &intValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_Reset, intValue);
	else
		Parameters->Set(NVSDK_NGX_Parameter_Reset, 0);

	if (InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &floatValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_MV_Scale_X, floatValue);
	else
	{
		spdlog::error("DLSSFeature::ProcessEvaluateParams no motion scale x!");
		Parameters->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
	}

	if (InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &floatValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_MV_Scale_Y, floatValue);
	else
	{
		spdlog::error("DLSSFeature::ProcessEvaluateParams no motion scale y!");
		Parameters->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
	}

	if (InParameters->Get(NVSDK_NGX_Parameter_TonemapperType, &uintValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_TonemapperType, uintValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &uintValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, uintValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &uintValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, uintValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &uintValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, uintValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &uintValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, uintValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, &uintValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, uintValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, &uintValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, uintValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_X, &uintValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_X, uintValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_Y, &uintValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_Y, uintValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X, &uintValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X, uintValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y, &uintValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y, uintValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, &uintValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, uintValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, &uintValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, uintValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &uintValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, uintValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &uintValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, uintValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &floatValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, floatValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Indicator_Invert_X_Axis, &uintValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_DLSS_Indicator_Invert_X_Axis, uintValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Indicator_Invert_Y_Axis, &uintValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_DLSS_Indicator_Invert_Y_Axis, uintValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, &floatValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, floatValue);
	else
		Parameters->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);

	if (InParameters->Get(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, &floatValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, floatValue);
}

void DLSSFeature::ProcessInitParams(const NVSDK_NGX_Parameter* InParameters)
{
	unsigned int uintValue;
	int intValue;

	if (InParameters->Get(NVSDK_NGX_Parameter_CreationNodeMask, &uintValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, uintValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_VisibilityNodeMask, &uintValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, uintValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, &intValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, intValue);

	if (InParameters->Get(NVSDK_NGX_Parameter_RTXValue, &intValue) == NVSDK_NGX_Result_Success)
		Parameters->Set(NVSDK_NGX_Parameter_RTXValue, intValue);

	// Create flags -----------------------------
	unsigned int featureFlags = 0;

	bool isHdr = false;
	bool mvLowRes = false;
	bool mvJittered = false;
	bool depthInverted = false;
	bool sharpening = false;
	bool autoExposure = false;

	if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &uintValue) == NVSDK_NGX_Result_Success)
	{
		spdlog::info("DLSSFeature::ProcessInitParams featureFlags {0:X}", uintValue);

		isHdr = (uintValue & NVSDK_NGX_DLSS_Feature_Flags_IsHDR);
		mvLowRes = (uintValue & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes);
		mvJittered = (uintValue & NVSDK_NGX_DLSS_Feature_Flags_MVJittered);
		depthInverted = (uintValue & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted);
		sharpening = (uintValue & NVSDK_NGX_DLSS_Feature_Flags_DoSharpening);
		autoExposure = (uintValue & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);
	}

	if (Config::Instance()->DepthInverted.value_or(depthInverted))
	{
		Config::Instance()->DepthInverted = true;
		featureFlags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
		spdlog::info("DLSSFeature::ProcessInitParams featureFlags (DepthInverted) {0:b}", featureFlags);
	}
	else
	{
		spdlog::info("DLSSFeature::ProcessInitParams featureFlags (!DepthInverted) {0:b}", featureFlags);
	}

	if (Config::Instance()->AutoExposure.value_or(autoExposure))
	{
		Config::Instance()->AutoExposure = true;
		featureFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
		spdlog::info("DLSSFeature::ProcessInitParams featureFlags (AutoExposure) {0:b}", featureFlags);
	}
	else
	{
		spdlog::info("DLSSFeature::ProcessInitParams featureFlags (!AutoExposure) {0:b}", featureFlags);
	}

	if (Config::Instance()->HDR.value_or(isHdr))
	{
		Config::Instance()->HDR = true;
		featureFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
		spdlog::info("DLSSFeature::ProcessInitParams featureFlags (HDR) {0:b}", featureFlags);
	}
	else
	{
		Config::Instance()->HDR = false;
		spdlog::info("DLSSFeature::ProcessInitParams featureFlags (!HDR) {0:b}", featureFlags);
	}

	if (Config::Instance()->JitterCancellation.value_or(mvJittered))
	{
		Config::Instance()->JitterCancellation = true;
		featureFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
		spdlog::info("DLSSFeature::ProcessInitParams featureFlags (JitterCancellation) {0:b}", featureFlags);
	}
	else
	{
		Config::Instance()->JitterCancellation = false;
		spdlog::info("DLSSFeature::ProcessInitParams featureFlags (!JitterCancellation) {0:b}", featureFlags);
	}

	if (Config::Instance()->DisplayResolution.value_or(!mvLowRes))
	{
		spdlog::info("DLSSFeature::ProcessInitParams featureFlags (!LowResMV) {0:b}", featureFlags);
	}
	else
	{
		featureFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
		spdlog::info("DLSSFeature::ProcessInitParams featureFlags (LowResMV) {0:b}", featureFlags);
	}

	if (Config::Instance()->OverrideSharpness.value_or(sharpening) && !(Config::Instance()->Api == NVNGX_DX12 && Config::Instance()->RcasEnabled.value_or(false)))
	{
		featureFlags |= NVSDK_NGX_DLSS_Feature_Flags_DoSharpening;
		spdlog::info("DLSSFeature::ProcessInitParams featureFlags (Sharpening) {0:b}", featureFlags);
	}
	else
	{
		spdlog::info("DLSSFeature::ProcessInitParams featureFlags (!Sharpening) {0:b}", featureFlags);
	}

	Parameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, featureFlags);

	// Resolution -----------------------------
	if (Config::Instance()->OutputScalingEnabled.value_or(false) && !Config::Instance()->DisplayResolution.value_or(false))
	{
		float ssMulti = Config::Instance()->OutputScalingMultiplier.value_or(1.5f);

		if (ssMulti < 0.5f)
		{
			ssMulti = 0.5f;
			Config::Instance()->OutputScalingMultiplier = ssMulti;
		}
		else if (ssMulti > 3.0f)
		{
			ssMulti = 3.0f;
			Config::Instance()->OutputScalingMultiplier = ssMulti;
		}

		_targetWidth = DisplayWidth() * ssMulti;
		_targetHeight = DisplayHeight() * ssMulti;
	}
	else
	{
		_targetWidth = DisplayWidth();
		_targetHeight = DisplayHeight();
	}

	Parameters->Set(NVSDK_NGX_Parameter_Width, RenderWidth());
	Parameters->Set(NVSDK_NGX_Parameter_Height, RenderHeight());
	Parameters->Set(NVSDK_NGX_Parameter_OutWidth, TargetWidth());
	Parameters->Set(NVSDK_NGX_Parameter_OutHeight, TargetHeight());

	unsigned int RenderPresetDLAA = 0;
	unsigned int RenderPresetUltraQuality = 0;
	unsigned int RenderPresetQuality = 0;
	unsigned int RenderPresetBalanced = 0;
	unsigned int RenderPresetPerformance = 0;
	unsigned int RenderPresetUltraPerformance = 0;

	InParameters->Get(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, &RenderPresetDLAA);
	InParameters->Get(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, &RenderPresetUltraQuality);
	InParameters->Get(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, &RenderPresetQuality);
	InParameters->Get(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, &RenderPresetBalanced);
	InParameters->Get(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, &RenderPresetPerformance);
	InParameters->Get(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, &RenderPresetUltraPerformance);

	if (Config::Instance()->RenderPresetOverride.value_or(false))
	{
		RenderPresetDLAA = Config::Instance()->RenderPresetDLAA.value_or(RenderPresetDLAA);
		RenderPresetUltraQuality = Config::Instance()->RenderPresetUltraQuality.value_or(RenderPresetUltraQuality);
		RenderPresetQuality = Config::Instance()->RenderPresetQuality.value_or(RenderPresetQuality);
		RenderPresetBalanced = Config::Instance()->RenderPresetBalanced.value_or(RenderPresetBalanced);
		RenderPresetPerformance = Config::Instance()->RenderPresetPerformance.value_or(RenderPresetPerformance);
		RenderPresetUltraPerformance = Config::Instance()->RenderPresetUltraPerformance.value_or(RenderPresetUltraPerformance);
	}

	if (RenderPresetDLAA < 0 || RenderPresetDLAA > 5)
		RenderPresetDLAA = 0;

	if (RenderPresetUltraQuality < 0 || RenderPresetUltraQuality > 5)
		RenderPresetUltraQuality = 0;

	if (RenderPresetQuality < 0 || RenderPresetQuality > 5)
		RenderPresetQuality = 0;

	if (RenderPresetBalanced < 0 || RenderPresetBalanced > 5)
		RenderPresetBalanced = 0;

	if (RenderPresetPerformance < 0 || RenderPresetPerformance > 5)
		RenderPresetPerformance = 0;

	if (RenderPresetUltraPerformance < 0 || RenderPresetUltraPerformance > 5)
		RenderPresetUltraPerformance = 0;

	Parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, RenderPresetDLAA);
	Parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, RenderPresetUltraQuality);
	Parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, RenderPresetQuality);
	Parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, RenderPresetBalanced);
	Parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, RenderPresetPerformance);
	Parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, RenderPresetUltraPerformance);
}

void DLSSFeature::ReadVersion()
{
	PFN_NVSDK_NGX_GetSnippetVersion _GetSnippetVersion = nullptr;

	_GetSnippetVersion = (PFN_NVSDK_NGX_GetSnippetVersion)DetourFindFunction("nvngx_dlss.dll", "NVSDK_NGX_GetSnippetVersion");

	if (_GetSnippetVersion != nullptr)
	{
		spdlog::trace("DLSSFeature::ReadVersion DLSS _GetSnippetVersion ptr: {0:X}", (ULONG64)_GetSnippetVersion);

		auto result = _GetSnippetVersion();

		_version.major = (result & 0x00FF0000) / 0x00010000;
		_version.minor = (result & 0x0000FF00) / 0x00000100;
		_version.patch = result & 0x000000FF / 0x00000001;

		spdlog::info("DLSSFeature::ReadVersion DLSS v{0}.{1}.{2} loaded.", _version.major, _version.minor, _version.patch);
		return;
	}

	spdlog::info("DLSSFeature::ReadVersion GetProcAddress for NVSDK_NGX_GetSnippetVersion failed!");
}

DLSSFeature::DLSSFeature(unsigned int handleId, const NVSDK_NGX_Parameter* InParameters) : IFeature(handleId, InParameters)
{
	if (NVNGXProxy::NVNGXModule() == nullptr)
		NVNGXProxy::InitNVNGX();

	if (NVNGXProxy::NVNGXModule() != nullptr && !Config::Instance()->DE_Available)
	{
		HookNvApi();
		HookNgxApi(NVNGXProxy::NVNGXModule());
	}

	_moduleLoaded = NVNGXProxy::NVNGXModule() != nullptr;
}

DLSSFeature::~DLSSFeature()
{
}

void DLSSFeature::Shutdown()
{
	if (NVNGXProxy::NVNGXModule() != nullptr)
		UnhookApis();
}