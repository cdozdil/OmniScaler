#pragma once
#include "DLSSFeature.h"
#include "../IFeature_Dx12.h"
#include "../../rcas/RCAS_Dx12.h"
#include <string>

class DLSSFeatureDx12 : public DLSSFeature, public IFeature_Dx12
{
private:
	float GetSharpness(const NVSDK_NGX_Parameter* InParameters);

protected:


public:
	bool Init(ID3D12Device* InDevice, ID3D12GraphicsCommandList* InCommandList, const NVSDK_NGX_Parameter* InParameters) override;
	bool Evaluate(ID3D12GraphicsCommandList* InCommandList, const NVSDK_NGX_Parameter* InParameters) override;

	static void Shutdown(ID3D12Device* InDevice);

	DLSSFeatureDx12(unsigned int InHandleId, const NVSDK_NGX_Parameter* InParameters);
	~DLSSFeatureDx12();
};