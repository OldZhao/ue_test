/*
* Copyright (c) 2020 NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA Corporation and its licensors retain all intellectual property and proprietary
* rights in and to this software, related documentation and any modifications thereto.
* Any use, reproduction, disclosure or distribution of this software and related
* documentation without an express license agreement from NVIDIA Corporation is strictly
* prohibited.
*
* TO THE MAXIMUM EXTENT PERMITTED BY APPLICABLE LAW, THIS SOFTWARE IS PROVIDED *AS IS*
* AND NVIDIA AND ITS SUPPLIERS DISCLAIM ALL WARRANTIES, EITHER EXPRESS OR IMPLIED,
* INCLUDING, BUT NOT LIMITED TO, IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
* PARTICULAR PURPOSE.  IN NO EVENT SHALL NVIDIA OR ITS SUPPLIERS BE LIABLE FOR ANY
* SPECIAL, INCIDENTAL, INDIRECT, OR CONSEQUENTIAL DAMAGES WHATSOEVER (INCLUDING, WITHOUT
* LIMITATION, DAMAGES FOR LOSS OF BUSINESS PROFITS, BUSINESS INTERRUPTION, LOSS OF
* BUSINESS INFORMATION, OR ANY OTHER PECUNIARY LOSS) ARISING OUT OF THE USE OF OR
* INABILITY TO USE THIS SOFTWARE, EVEN IF NVIDIA HAS BEEN ADVISED OF THE POSSIBILITY OF
* SUCH DAMAGES.
*/

#include "DLSSUpscaler.h"


#include "DLSSUpscalerPrivate.h"
#include "DLSSUpscalerHistory.h"
#include "DLSSSettings.h"

#include "VelocityCombinePass.h"

#include "PostProcess/SceneRenderTargets.h"
#include "PostProcess/PostProcessing.h"
#include "SceneTextureParameters.h"
#include "ScreenPass.h"

#include "RayTracing/RaytracingOptions.h"

#include "LegacyScreenPercentageDriver.h"

#include <string>
#include <fstream>
//#include <Runtime/Windows/D3D11RHI/Public/D3D11Resources.h>
//#include <ThirdParty/NGX/Include/nvsdk_ngx.h>


#define LOCTEXT_NAMESPACE "FDLSSModule"


static TAutoConsoleVariable<int32> CVarNGXDLSSEnable(
	TEXT("r.NGX.DLSS.Enable"), 1,
	TEXT("Enable/Disable DLSS entirely."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNGXDLSSAutomationTesting(
	TEXT("r.NGX.DLSS.AutomationTesting"), 0,
	TEXT("Whether the NGX library should be loaded when GIsAutomationTesting is true.(default is false)\n")
	TEXT("Must be set to true before startup. This can be enabled for cases where running automation testing with DLSS desired"),
	ECVF_ReadOnly);

static TAutoConsoleVariable<int32> CVarNGXDLSSPerfQualitySetting(
	TEXT("r.NGX.DLSS.Quality"),
	-1,
	TEXT("DLSS Performance/Quality setting. Not all modes might be supported at runtime, in this case Balanced mode is used as a fallback\n")
	TEXT(" -2: Ultra Performance\n")
	TEXT(" -1: Performance (default)\n")
	TEXT("  0: Balanced\n")
	TEXT("  1: Quality\n")
	TEXT("  2: Ultra Quality\n"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<bool> CVarNGXDLSSAutoQualitySetting(
	TEXT("r.NGX.DLSS.Quality.Auto"), 0,
	TEXT("Enable/Disable DLSS automatically selecting the DLSS quality mode based on the render resolution"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarNGXDLSSSharpness(
	TEXT("r.NGX.DLSS.Sharpness"),
	0.0f,
	TEXT("-1.0 to 1.0: Softening/sharpening to apply to the DLSS pass. Negative values soften the image, positive values sharpen. (default: 0.0f)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNGXDLSSDilateMotionVectors(
	TEXT("r.NGX.DLSS.DilateMotionVectors"),
	1,
	TEXT(" 0: pass low resolution motion vectors into DLSS\n")
	TEXT(" 1: pass dilated high resolution motion vectors into DLSS. This can help with improving image quality of thin details. (default)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNGXDLSSAutoExposure(
	TEXT("r.NGX.DLSS.AutoExposure"), 0,
	TEXT("0: Use the engine-computed exposure value for input images to DLSS (default)\n")
	TEXT("1: Enable DLSS internal auto-exposure instead of the application provided one - enabling this can alleviate effects such as ghosting in darker scenes.\n"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNGXDLSSReleaseMemoryOnDelete(
	TEXT("r.NGX.DLSS.ReleaseMemoryOnDelete"), 
	1,
	TEXT("Enabling/disable releasing DLSS related memory on the NGX side when DLSS features get released.(default=1)"),
	ECVF_RenderThreadSafe);

DECLARE_GPU_STAT(DLSS)

BEGIN_SHADER_PARAMETER_STRUCT(FDLSSShaderParameters, )

// Input images
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorInput)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthInput)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, EyeAdaptation)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneVelocityInput)


// Output images
RDG_TEXTURE_ACCESS(SceneColorOutput, ERHIAccess::UAVCompute)

END_SHADER_PARAMETER_STRUCT()


FIntPoint FDLSSPassParameters::GetOutputExtent() const
{
	check(Validate());
	check(SceneColorInput);

	FIntPoint InputExtent = SceneColorInput->Desc.Extent;

	FIntPoint QuantizedPrimaryUpscaleViewSize;
	QuantizeSceneBufferSize(OutputViewRect.Size(), QuantizedPrimaryUpscaleViewSize);

	return FIntPoint(
		FMath::Max(InputExtent.X, QuantizedPrimaryUpscaleViewSize.X),
		FMath::Max(InputExtent.Y, QuantizedPrimaryUpscaleViewSize.Y));
}

bool FDLSSPassParameters::Validate() const
{
	checkf(OutputViewRect.Min == FIntPoint::ZeroValue,TEXT("The DLSS OutputViewRect %dx%d must be non-zero"), OutputViewRect.Min.X, OutputViewRect.Min.Y);
	return true;
}

const TCHAR* FDLSSUpscaler::GetDebugName() const
{
	return TEXT("FDLSSUpscaler");
}


static NVSDK_NGX_PerfQuality_Value ToNGXQuality(EDLSSQualityMode Quality)
{
	static_assert(int32(EDLSSQualityMode::NumValues) == 5, "dear DLSS plugin NVIDIA developer, please update this code to handle the new EDLSSQualityMode enum values");
	switch (Quality)
	{
		case EDLSSQualityMode::UltraPerformance:
			return NVSDK_NGX_PerfQuality_Value_UltraPerformance;

		default:
			checkf(false, TEXT("ToNGXQuality should not be called with an out of range EDLSSQualityMode from the higher level code"));
		case EDLSSQualityMode::Performance:
			return NVSDK_NGX_PerfQuality_Value_MaxPerf;

		case EDLSSQualityMode::Balanced:
			return NVSDK_NGX_PerfQuality_Value_Balanced;

		case EDLSSQualityMode::Quality:
			return NVSDK_NGX_PerfQuality_Value_MaxQuality;
		
		case EDLSSQualityMode::UltraQuality:
			return NVSDK_NGX_PerfQuality_Value_UltraQuality;
	}
}

NGXRHI* FDLSSUpscaler::NGXRHIExtensions;
TStaticArray <TSharedPtr<FDLSSUpscaler>, uint32(EDLSSQualityMode::NumValues)> FDLSSUpscaler::DLSSUpscalerInstancesPerViewFamily;
float FDLSSUpscaler::MinResolutionFraction = TNumericLimits <float>::Max();
float FDLSSUpscaler::MaxResolutionFraction = TNumericLimits <float>::Min();


uint32 FDLSSUpscaler::NumRuntimeQualityModes = 0;
TArray<FDLSSOptimalSettings> FDLSSUpscaler::ResolutionSettings;


FDLSSUpscaler* FDLSSUpscaler::GetUpscalerInstanceForViewFamily(const FDLSSUpscaler* InUpscaler, EDLSSQualityMode InQualityMode)
{
	uint32 ArrayIndex = (int32)ToNGXQuality(InQualityMode);
	if (!DLSSUpscalerInstancesPerViewFamily[ArrayIndex])
	{
		DLSSUpscalerInstancesPerViewFamily[ArrayIndex] = MakeShared<FDLSSUpscaler>(InUpscaler, InQualityMode);

	}
	return DLSSUpscalerInstancesPerViewFamily[ArrayIndex].Get();
}

bool FDLSSUpscaler::IsValidUpscalerInstance(const ITemporalUpscaler* InUpscaler)
{
	// DLSSUpscalerInstancesPerViewFamily gets lazily initialized, but we don't want to accidentally treat nullptr as a valid
	// upscaler instance, when we want to check (e.g. in the denoiser) whether DLSS is actually active for the viewfamily
	if (InUpscaler == nullptr)
	{
		return false;
	}

	for (auto UpscalerInstance : DLSSUpscalerInstancesPerViewFamily)
	{
		if (UpscalerInstance.Get() == InUpscaler)
		{
			return true;
		}
	}
	return false;
}

bool FDLSSUpscaler::IsAutoQualityMode()
{
	return CVarNGXDLSSAutoQualitySetting.GetValueOnAnyThread();
}

void FDLSSUpscaler::SetAutoQualityMode(bool bAutoQualityMode)
{
	check(IsInGameThread());
	CVarNGXDLSSAutoQualitySetting->Set(bAutoQualityMode, ECVF_SetByCommandline);
}

// make copy & assign quality mode
FDLSSUpscaler::FDLSSUpscaler(const FDLSSUpscaler* InUpscaler, EDLSSQualityMode InQualityMode)
	: FDLSSUpscaler(*InUpscaler)
{
	DLSSQualityMode = InQualityMode;
	check(NGXRHIExtensions);
}


FDLSSUpscaler::FDLSSUpscaler(NGXRHI* InNGXRHIExtensions)
	
{
	UE_LOG(LogDLSS, Log, TEXT("%s Enter"), ANSI_TO_TCHAR(__FUNCTION__));
	
	
	checkf(!NGXRHIExtensions, TEXT("static member NGXRHIExtensions should only be assigned once by this ctor when called during module startup") );
	NGXRHIExtensions = InNGXRHIExtensions;

	ResolutionSettings.Init(FDLSSOptimalSettings(), int32(EDLSSQualityMode::NumValues));

	static_assert(int32(EDLSSQualityMode::NumValues) == 5, "dear DLSS plugin NVIDIA developer, please update this code to handle the new EDLSSQualityMode enum values");
	for (auto QualityMode : { EDLSSQualityMode::UltraPerformance,  EDLSSQualityMode::Performance , EDLSSQualityMode::Balanced, EDLSSQualityMode::Quality,  EDLSSQualityMode::UltraQuality })
	{
		check(ToNGXQuality(QualityMode) < ResolutionSettings.Num());
		check(ToNGXQuality(QualityMode) >= 0);

		FDLSSOptimalSettings OptimalSettings = NGXRHIExtensions->GetDLSSOptimalSettings(ToNGXQuality(QualityMode));
		
		ResolutionSettings[ToNGXQuality(QualityMode)] = OptimalSettings;

		// we only consider non-fixed resolutions for the overall min / max resolution fraction
		if (OptimalSettings.bIsSupported && !OptimalSettings.IsFixedResolution())
		{
			// We use OptimalSettings.OptimalResolutionFraction to avoid getting to "floating point close" to OptimalSettings.{MinMax}ResolutionFraction) 
			MinResolutionFraction = FMath::Min(MinResolutionFraction, OptimalSettings.OptimalResolutionFraction);
			MaxResolutionFraction = FMath::Max(MaxResolutionFraction, OptimalSettings.OptimalResolutionFraction);
			++NumRuntimeQualityModes;
		}

		UE_LOG(LogDLSS, Log, TEXT("QualityMode %d: bSupported = %u, ResolutionFraction = %.4f. MinResolutionFraction=%.4f,  MaxResolutionFraction %.4f"),
			QualityMode, OptimalSettings.bIsSupported, OptimalSettings.OptimalResolutionFraction, OptimalSettings.MinResolutionFraction, OptimalSettings.MaxResolutionFraction);
	}

	// the DLSS module will report DLSS as not supported if there are no supported quality modes at runtime
	UE_LOG(LogDLSS, Log, TEXT("NumRuntimeQualityModes=%u, MinResolutionFraction=%.4f,  MaxResolutionFraction=%.4f"), NumRuntimeQualityModes, MinResolutionFraction, MaxResolutionFraction);

	// Higher levels of the code (e.g. UI) should check whether each mode is actually supported
	// But for now verify early that the DLSS 2.0 modes are supported. Those checks could be removed in the future
	check(IsQualityModeSupported(EDLSSQualityMode::Performance));
	check(IsQualityModeSupported(EDLSSQualityMode::Balanced));
	check(IsQualityModeSupported(EDLSSQualityMode::Quality));


	UE_LOG(LogDLSS, Log, TEXT("%s Leave"), ANSI_TO_TCHAR(__FUNCTION__));
}

FDLSSUpscaler::~FDLSSUpscaler()
{
	UE_LOG(LogDLSS, Log, TEXT("%s Enter"), ANSI_TO_TCHAR(__FUNCTION__));

	UE_LOG(LogDLSS, Log, TEXT("%s Leave"), ANSI_TO_TCHAR(__FUNCTION__));
}

// this gets explicitly called during module shutdown
void FDLSSUpscaler::ReleaseStaticResources()
{
	UE_LOG(LogDLSS, Log, TEXT("%s Enter"), ANSI_TO_TCHAR(__FUNCTION__));
	ResolutionSettings.Empty();
	for (auto& UpscalerInstance : DLSSUpscalerInstancesPerViewFamily)
	{
		UpscalerInstance.Reset();
	}
	
	UE_LOG(LogDLSS, Log, TEXT("%s Leave"), ANSI_TO_TCHAR(__FUNCTION__));
}

static void DumpTexture(std::string Filename, FRHITexture* Texture, FRHICommandListImmediate& RHICmdList)
{
	FRHITexture2D* TexRef2D = Texture->GetTexture2D();
	uint32 LolStride = 0;
	char* TextureDataPtr = (char*)RHICmdList.LockTexture2D(TexRef2D, 0, EResourceLockMode::RLM_ReadOnly, LolStride, false);

	std::ofstream b_stream(Filename.c_str(), std::fstream::out | std::fstream::binary);
	EPixelFormat TextureFormat_ = Texture->GetFormat();

	int bytes = TexRef2D->GetSizeX() * TexRef2D->GetSizeY();
	if (TextureFormat_ == EPixelFormat::PF_FloatRGBA) {
		bytes = bytes * 4 * 2;
	}
	else if (TextureFormat_ == EPixelFormat::PF_DepthStencil) {
		bytes = bytes * 1 * 4;
	}
	else if (TextureFormat_ == EPixelFormat::PF_G16R16F) {
		bytes = bytes * 2 * 2;
	}

	if (b_stream) {
		b_stream.write(TextureDataPtr, bytes);
	}
	b_stream.close();
	RHICmdList.UnlockTexture2D(TexRef2D, 0, false);
}

int count = 0;
static void TextureWriting_RenderThread(
	FRHICommandListImmediate& RHICmdList, FRHITexture* Texture, FRHITexture* TextureInput, FRHITexture* TextureDepth, FRHITexture* TextureVelocity)
{
	check(IsInRenderingThread());
	if (Texture == nullptr)
	{
		UE_LOG(LogDLSS, Warning, TEXT("Texture is null"));
		return;
	}
	if (TextureInput == nullptr)
	{
		UE_LOG(LogDLSS, Warning, TEXT("TextureInput is null"));
		return;
	}

	FRHITexture2D* TexRef2D = Texture->GetTexture2D();
	int sizeX = TexRef2D->GetSizeX();
	int sizeY = TexRef2D->GetSizeY();
	std::string PathRoot = "D:/pc_code/data/DLSS_" + std::to_string(count) + "_" + std::to_string(sizeX) + "_" + std::to_string(sizeY);
	std::string FilenameOutput = PathRoot + "_output.txt";
	std::string FilenameInput = PathRoot + "_input.txt";
	std::string FilenameDepth = PathRoot + "_depth.txt";
	FRHITexture2D* TexRefVelocity2D = TextureVelocity->GetTexture2D();
	sizeX = TexRefVelocity2D->GetSizeX();
	sizeY = TexRefVelocity2D->GetSizeY();
	std::string VelocityPathRoot = "D:/pc_code/data/DLSS_" + std::to_string(count) + "_" + std::to_string(sizeX) + "_" + std::to_string(sizeY);
	std::string FilenameVelocity = VelocityPathRoot + "_velocity.txt";
	DumpTexture(FilenameOutput, Texture, RHICmdList);
	DumpTexture(FilenameInput, TextureInput, RHICmdList);
	DumpTexture(FilenameDepth, TextureDepth, RHICmdList);
	DumpTexture(FilenameVelocity, TextureVelocity, RHICmdList);
	count++;
}

void FDLSSUpscaler::AddPasses(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FPassInputs& PassInputs,
	FRDGTextureRef* OutSceneColorTexture,
	FIntRect* OutSceneColorViewRect,
	FRDGTextureRef* OutSceneColorHalfResTexture,
	FIntRect* OutSceneColorHalfResViewRect) const
{
	// For TAAU, this can happen with screen percentages larger than 100%, so not something that DLSS viewports are setup with
	checkf(!PassInputs.bAllowDownsampleSceneColor,TEXT("The DLSS plugin does not support downsampling the scenecolor. Please set r.TemporalAA.AllowDownsampling=0"));
	checkf(View.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::TemporalUpscale, TEXT("DLSS requires TemporalUpscale. If you hit this assert, please set r.TemporalAA.Upscale=1"));


	

	const FTemporalAAHistory& InputHistory = View.PrevViewInfo.TemporalAAHistory;
	const TRefCountPtr<ICustomTemporalAAHistory> InputCustomHistory = View.PrevViewInfo.CustomTemporalAAHistory;

	FTemporalAAHistory* OutputHistory = View.ViewState ? &(View.ViewState->PrevFrameViewInfo.TemporalAAHistory) : nullptr;
	TRefCountPtr < ICustomTemporalAAHistory >* OutputCustomHistory = View.ViewState ? &(View.ViewState->PrevFrameViewInfo.CustomTemporalAAHistory) : nullptr;

	
	FDLSSPassParameters DLSSParameters(View);
	const FIntRect SecondaryViewRect = DLSSParameters.OutputViewRect;
	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, DLSS);
		RDG_EVENT_SCOPE(GraphBuilder, "DLSS");

		const bool bDilateMotionVectors = CVarNGXDLSSDilateMotionVectors.GetValueOnRenderThread() != 0;

		FRDGTextureRef CombinedVelocityTexture = AddVelocityCombinePass(GraphBuilder, View, PassInputs.SceneDepthTexture, PassInputs.SceneVelocityTexture, bDilateMotionVectors);

		DLSSParameters.SceneColorInput = PassInputs.SceneColorTexture;
		DLSSParameters.SceneVelocityInput = CombinedVelocityTexture;
		DLSSParameters.SceneDepthInput = PassInputs.SceneDepthTexture;
		DLSSParameters.bHighResolutionMotionVectors = bDilateMotionVectors;
		const FDLSSOutputs DLSSOutputs = AddDLSSPass(
			GraphBuilder,
			View,
			DLSSParameters,
			InputHistory,
			OutputHistory,
			InputCustomHistory,
			OutputCustomHistory
		);

		FRDGTextureRef SceneColorTexture = DLSSOutputs.SceneColor;

		*OutSceneColorTexture = SceneColorTexture;
		*OutSceneColorViewRect = SecondaryViewRect;

		*OutSceneColorHalfResTexture = nullptr;
		*OutSceneColorHalfResViewRect = FIntRect(FIntPoint::ZeroValue, FIntPoint::ZeroValue);
	}
}

FDLSSOutputs FDLSSUpscaler::AddDLSSPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FDLSSPassParameters& Inputs,
	const FTemporalAAHistory& InputHistory,
	FTemporalAAHistory* OutputHistory,
	const TRefCountPtr<ICustomTemporalAAHistory> InputCustomHistoryInterface,
	TRefCountPtr<ICustomTemporalAAHistory>* OutputCustomHistoryInterface
) const
{
	check(IsValidUpscalerInstance(this));
	check(IsDLSSActive());
	const FDLSSUpscalerHistory* InputCustomHistory = static_cast<const FDLSSUpscalerHistory*>(InputCustomHistoryInterface.GetReference());

	const bool bCameraCut = !InputHistory.IsValid() || View.bCameraCut || !OutputHistory;
	const FIntPoint OutputExtent = Inputs.GetOutputExtent();

	const FIntRect SrcRect = Inputs.InputViewRect;
	const FIntRect DestRect = Inputs.OutputViewRect;

	const float ScaleX = float(SrcRect.Width()) / float(DestRect.Width());
	const float ScaleY = float(SrcRect.Height()) / float(DestRect.Height());

	if (InputHistory.RT[0].IsValid())
	{
		FRHITexture* historyTarget1 = InputHistory.RT[0]->GetRenderTargetItem().TargetableTexture;
		FRHITexture* historyTargetInput = InputHistory.RT[1]->GetRenderTargetItem().TargetableTexture;
		FRHITexture* historyTargetDepth = InputHistory.RT[2]->GetRenderTargetItem().TargetableTexture;
		FRHITexture* historyTargetVelocity = InputHistory.RT[3]->GetRenderTargetItem().TargetableTexture;

		ENQUEUE_RENDER_COMMAND(CaptureCommand)(
			[historyTarget1, historyTargetInput, historyTargetDepth, historyTargetVelocity](FRHICommandListImmediate& RHICmdList)
			{
				TextureWriting_RenderThread(RHICmdList, historyTarget1, historyTargetInput, historyTargetDepth, historyTargetVelocity);
			});

	}

	// FDLSSUpscaler::SetupMainGameViewFamily or FDLSSUpscalerEditor::SetupEditorViewFamily 
	// set DLSSQualityMode by setting an FDLSSUpscaler on the ViewFamily (from the pool in DLSSUpscalerInstancesPerViewFamily)
	
	checkf(DLSSQualityMode != EDLSSQualityMode::NumValues, TEXT("Invalid Quality mode, not initialized"));
	checkf(IsQualityModeSupported(DLSSQualityMode), TEXT("%u is not a valid Quality mode"), DLSSQualityMode);

	// This assert can accidentally hit with small viewrect dimensions (e.g. when resizing an editor view) due to floating point rounding & quantization issues
	// e.g. with 33% screen percentage at 1000 DestRect dimension we get 333/1000 = 0.33 but at 10 DestRect dimension we get 3/10 0.3, thus the assert hits
	
	checkf(DestRect.Width()  < 100 || GetMinResolutionFractionForQuality(DLSSQualityMode) - 0.01f <= ScaleX && ScaleX <= GetMaxResolutionFractionForQuality(DLSSQualityMode) + 0.01f, TEXT("The current resolution fraction %f is out of the supported DLSS range [%f ... %f] for quality mode %d."), ScaleX, GetMinResolutionFractionForQuality(DLSSQualityMode), GetMaxResolutionFractionForQuality(DLSSQualityMode), DLSSQualityMode);
	checkf(DestRect.Height() < 100 || GetMinResolutionFractionForQuality(DLSSQualityMode) - 0.01f <= ScaleY && ScaleY <= GetMaxResolutionFractionForQuality(DLSSQualityMode) + 0.01f, TEXT("The current resolution fraction %f is out of the supported DLSS range [%f ... %f] for quality mode %d."), ScaleY, GetMinResolutionFractionForQuality(DLSSQualityMode), GetMaxResolutionFractionForQuality(DLSSQualityMode), DLSSQualityMode);

	const TCHAR* PassName = TEXT("MainUpsampling");

	// Create outputs
	FDLSSOutputs Outputs;
	{
		FRDGTextureDesc SceneColorDesc = FRDGTextureDesc::Create2D(
			OutputExtent,
			PF_FloatRGBA,
			FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_UAV);

		const TCHAR* OutputName = TEXT("DLSSOutputSceneColor");

		Outputs.SceneColor = GraphBuilder.CreateTexture(
			SceneColorDesc,
			OutputName);
	}

	FDLSSStateRef DLSSState = (InputCustomHistory && InputCustomHistory->DLSSState) ? InputCustomHistory->DLSSState : MakeShared<FDLSSState, ESPMode::ThreadSafe>();
	{
		FDLSSShaderParameters* PassParameters = GraphBuilder.AllocParameters<FDLSSShaderParameters>();

		// Set up common shader parameters
		const FIntPoint InputExtent = Inputs.SceneColorInput->Desc.Extent;
		const FIntRect InputViewRect = Inputs.InputViewRect;
		const FIntRect OutputViewRect = Inputs.OutputViewRect;

		// Input buffer shader parameters
		{
			PassParameters->SceneColorInput = Inputs.SceneColorInput;
			PassParameters->SceneDepthInput = Inputs.SceneDepthInput;
			PassParameters->SceneVelocityInput = Inputs.SceneVelocityInput;
			PassParameters->EyeAdaptation = GetEyeAdaptationTexture(GraphBuilder, View);
		}

		// Outputs 
		{
			PassParameters->SceneColorOutput = Outputs.SceneColor;
		}

		const FVector2D JitterOffset = View.TemporalJitterPixels;
		const float DeltaWorldTime = View.Family->DeltaWorldTime;

		const float PreExposure = View.PreExposure;
		const bool bUseAutoExposure = CVarNGXDLSSAutoExposure.GetValueOnRenderThread() != 0;

		const bool bReleaseMemoryOnDelete = CVarNGXDLSSReleaseMemoryOnDelete.GetValueOnRenderThread() != 0;

		const float Sharpness = FMath::Clamp(CVarNGXDLSSSharpness.GetValueOnRenderThread(), -1.0f, 1.0f);
		NGXRHI* LocalNGXRHIExtensions = this->NGXRHIExtensions;
		const int32 NGXPerfQuality = ToNGXQuality(DLSSQualityMode);
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("DLSS %s%s %dx%d -> %dx%d",
				PassName,
				Sharpness != 0.0f ? TEXT(" Sharpen") : TEXT(""),
				SrcRect.Width(), SrcRect.Height(),
				DestRect.Width(), DestRect.Height()),
			PassParameters,
			ERDGPassFlags::Compute | ERDGPassFlags::Raster | ERDGPassFlags::SkipRenderPass,
			[LocalNGXRHIExtensions, PassParameters, Inputs, bCameraCut, JitterOffset, DeltaWorldTime, PreExposure, Sharpness, NGXPerfQuality, DLSSState, bUseAutoExposure, bReleaseMemoryOnDelete](FRHICommandListImmediate& RHICmdList)
		{
			FRHIDLSSArguments DLSSArguments;
			FMemory::Memzero(&DLSSArguments, sizeof(DLSSArguments));

			// input parameters
			DLSSArguments.SrcRect = Inputs.InputViewRect;
			DLSSArguments.DestRect = Inputs.OutputViewRect;

			DLSSArguments.Sharpness = Sharpness;
			DLSSArguments.bReset = bCameraCut;
			DLSSArguments.JitterOffset = JitterOffset;

			DLSSArguments.MotionVectorScale = FVector2D(1.0f, 1.0f);
			DLSSArguments.bHighResolutionMotionVectors = Inputs.bHighResolutionMotionVectors;
			DLSSArguments.DeltaTime = DeltaWorldTime;
			DLSSArguments.bReleaseMemoryOnDelete = bReleaseMemoryOnDelete;

			DLSSArguments.PerfQuality = NGXPerfQuality;

			check(PassParameters->SceneColorInput);
			PassParameters->SceneColorInput->MarkResourceAsUsed();
			DLSSArguments.InputColor = PassParameters->SceneColorInput->GetRHI();
					

			check(PassParameters->SceneVelocityInput);
			PassParameters->SceneVelocityInput->MarkResourceAsUsed();
			DLSSArguments.InputMotionVectors = PassParameters->SceneVelocityInput->GetRHI();

			check(PassParameters->SceneDepthInput);
			PassParameters->SceneDepthInput->MarkResourceAsUsed();
			DLSSArguments.InputDepth = PassParameters->SceneDepthInput->GetRHI();

			check(PassParameters->EyeAdaptation);
			PassParameters->EyeAdaptation->MarkResourceAsUsed();
			DLSSArguments.InputExposure = PassParameters->EyeAdaptation->GetRHI();
			DLSSArguments.PreExposure = PreExposure;
			

			// output images
			check(PassParameters->SceneColorOutput);
			PassParameters->SceneColorOutput->MarkResourceAsUsed();
			DLSSArguments.OutputColor = PassParameters->SceneColorOutput->GetRHI();
			DLSSArguments.bUseAutoExposure = bUseAutoExposure;
			RHICmdList.TransitionResource(ERHIAccess::UAVMask, DLSSArguments.OutputColor);
			RHICmdList.EnqueueLambda(
				[LocalNGXRHIExtensions, DLSSArguments, DLSSState](FRHICommandListImmediate& Cmd)
			{
				LocalNGXRHIExtensions->ExecuteDLSS(Cmd, DLSSArguments, DLSSState);
			});
		});
	}

	if (!View.bStatePrevViewInfoIsReadOnly && OutputHistory)
	{
		OutputHistory->SafeRelease();

		GraphBuilder.QueueTextureExtraction(Outputs.SceneColor, &OutputHistory->RT[0]);
		GraphBuilder.QueueTextureExtraction(Inputs.SceneColorInput, &OutputHistory->RT[1]);
		GraphBuilder.QueueTextureExtraction(Inputs.SceneDepthInput, &OutputHistory->RT[2]);
		GraphBuilder.QueueTextureExtraction(Inputs.SceneVelocityInput, &OutputHistory->RT[3]);

		OutputHistory->ViewportRect = DestRect;
		OutputHistory->ReferenceBufferSize = OutputExtent;
	}


	FReadSurfaceDataFlags ReadDataFlags;
	ReadDataFlags.SetLinearToGamma(false);
	ReadDataFlags.SetOutputStencil(false);
	ReadDataFlags.SetMip(0);

	
	FRDGTexture* OutputTexture = Outputs.SceneColor;
	AddReadbackTexturePass(GraphBuilder, RDG_EVENT_NAME("SaveBitmap"), OutputTexture,
		[OutputTexture, OutputExtent, ReadDataFlags](FRHICommandListImmediate& RHICmdList)
		{
			TArray<FFloat16Color> Bitmap;

			RHICmdList.ReadSurfaceFloatData(OutputTexture->GetRHI(), FIntRect(0, 0, OutputExtent.X, OutputExtent.Y), Bitmap, ReadDataFlags);

			uint32 ExtendXWithMSAA = Bitmap.Num() / OutputExtent.Y;
			
			std::string PathRoot = "D:/pc_code/data/map_DLSS_" + std::to_string(count) + "_" + std::to_string(OutputExtent.X) + "_" + std::to_string(OutputExtent.Y);
			std::string Filename = PathRoot + "_output.txt";
			int bytes = OutputExtent.X * OutputExtent.Y * 4 * 2;
			std::ofstream b_stream(Filename.c_str(), std::fstream::out | std::fstream::binary);

			if (b_stream) {
				b_stream.write((char*)Bitmap.GetData(), bytes);
			}
			b_stream.close();

	
		});

	FRDGTexture* InputTexture = Inputs.SceneColorInput;
	AddReadbackTexturePass(GraphBuilder, RDG_EVENT_NAME("SaveBitmapInput"), InputTexture,
		[InputTexture, SrcRect, ReadDataFlags](FRHICommandListImmediate& RHICmdList)
		{
			TArray<FFloat16Color> Bitmap;

			RHICmdList.ReadSurfaceFloatData(InputTexture->GetRHI(), SrcRect, Bitmap, ReadDataFlags);

			uint32 ExtendXWithMSAA = Bitmap.Num() / SrcRect.Height();


			std::string PathRoot = "D:/pc_code/data/map_DLSS_" + std::to_string(count) + "_" + std::to_string(SrcRect.Width()) + "_" + std::to_string(SrcRect.Height());
			std::string Filename = PathRoot + "_input.txt";
			int bytes = SrcRect.Width() * SrcRect.Height() * 4 * 2;
			std::ofstream b_stream(Filename.c_str(), std::fstream::out | std::fstream::binary);

			if (b_stream) {
				b_stream.write((char*)Bitmap.GetData(), bytes);
			}
			b_stream.close();


		});


	if (!View.bStatePrevViewInfoIsReadOnly && OutputCustomHistoryInterface)
	{
		if (!OutputCustomHistoryInterface->GetReference())
		{
			(*OutputCustomHistoryInterface) = new FDLSSUpscalerHistory(DLSSState);
		}
	}
	return Outputs;
}

void FDLSSUpscaler::Tick(FRHICommandListImmediate& RHICmdList)
{
	check(NGXRHIExtensions);
	check(IsInRenderingThread());
	// Pass it over to the RHI thread which handles the lifetime of the NGX DLSS resources
	RHICmdList.EnqueueLambda(
		[this](FRHICommandListImmediate& Cmd)
	{
		NGXRHIExtensions->TickPoolElements();
	});
}

bool FDLSSUpscaler::IsQualityModeSupported(EDLSSQualityMode InQualityMode) const
{
	return ResolutionSettings[ToNGXQuality(InQualityMode)].bIsSupported;
}

bool FDLSSUpscaler::IsDLSSActive() const
{
	static const auto CVarTemporalAAUpscaler = IConsoleManager::Get().FindConsoleVariable(TEXT("r.TemporalAA.Upscaler"));
	const bool bDLSSActive = ((GTemporalUpscaler == this) || IsValidUpscalerInstance(this)) &&
		CVarTemporalAAUpscaler && (CVarTemporalAAUpscaler->GetInt() != 0) &&
		(CVarNGXDLSSEnable.GetValueOnAnyThread() != 0);
	return bDLSSActive;
}


void FDLSSUpscaler::SetupMainGameViewFamily(FSceneViewFamily& ViewFamily)
{
	const bool bDLSSActiveWithAutomation = !GIsAutomationTesting || (GIsAutomationTesting && (CVarNGXDLSSAutomationTesting.GetValueOnAnyThread() != 0));
	
	if (IsDLSSActive() && bDLSSActiveWithAutomation)
	{
		checkf(GTemporalUpscaler == this, TEXT("GTemporalUpscaler is not set to a DLSS upscaler . Please check that only one upscaling plugin is active."));
		checkf(GCustomStaticScreenPercentage == this, TEXT("GCustomStaticScreenPercentage is not set to a DLSS upscaler. Please check that only one upscaling plugin is active."));

		if (!GIsEditor || (GIsEditor && GIsPlayInEditorWorld && EnableDLSSInPlayInEditorViewports()))
		{
			EDLSSQualityMode DLSSQuality;
			if (IsAutoQualityMode())
			{
				TOptional<EDLSSQualityMode> MaybeDLSSQuality = GetAutoQualityModeFromViewFamily(ViewFamily);
				if (!MaybeDLSSQuality.IsSet())
				{
					return;
				}
				else
				{
					DLSSQuality = MaybeDLSSQuality.GetValue();
				}
			}
			else
			{
				DLSSQuality = GetSupportedQualityModeFromCVarValue(CVarNGXDLSSPerfQualitySetting.GetValueOnGameThread());
			}

			ViewFamily.SetTemporalUpscalerInterface(GetUpscalerInstanceForViewFamily(this, DLSSQuality));

			if (ViewFamily.EngineShowFlags.ScreenPercentage && !ViewFamily.GetScreenPercentageInterface())
			{

				const float ResolutionFraction = GetOptimalResolutionFractionForQuality(DLSSQuality);
				ViewFamily.SetScreenPercentageInterface(new FLegacyScreenPercentageDriver(
					ViewFamily, ResolutionFraction,
					/* AllowPostProcessSettingsScreenPercentage = */  false));
			}
		}
	}
}

#if DLSS_ENGINE_SUPPORTS_CSSPD
void FDLSSUpscaler::SetupViewFamily(FSceneViewFamily& ViewFamily, TSharedPtr<ICustomStaticScreenPercentageData> InScreenPercentageDataInterface)
{
	check(InScreenPercentageDataInterface.IsValid());
	TSharedPtr<FDLSSViewportQualitySetting> ScreenPercentageData = StaticCastSharedPtr<FDLSSViewportQualitySetting>(InScreenPercentageDataInterface);
	
	EDLSSQualityMode Quality = static_cast<EDLSSQualityMode>(ScreenPercentageData->QualitySetting);
	if (!IsQualityModeSupported(Quality))
	{
		UE_LOG(LogDLSS, Warning, TEXT("DLSS Quality mode is not supported %d"), Quality);
		return;
	}
	const bool bDLSSActiveWithAutomation = !GIsAutomationTesting || (GIsAutomationTesting && (CVarNGXDLSSAutomationTesting.GetValueOnAnyThread() != 0));
	if (IsDLSSActive() && bDLSSActiveWithAutomation)
	{
		checkf(GTemporalUpscaler == this, TEXT("GTemporalUpscaler is not set to a DLSS upscaler . Please check that only one upscaling plugin is active."));
		checkf(GCustomStaticScreenPercentage == this, TEXT("GCustomStaticScreenPercentage is not set to a DLSS upscaler. Please check that only one upscaling plugin is active."));

		ViewFamily.SetTemporalUpscalerInterface(GetUpscalerInstanceForViewFamily(this, Quality));

		if (ViewFamily.EngineShowFlags.ScreenPercentage && !ViewFamily.GetScreenPercentageInterface())
		{
			const float ResolutionFraction = GetOptimalResolutionFractionForQuality(Quality);
			ViewFamily.SetScreenPercentageInterface(new FLegacyScreenPercentageDriver(
				ViewFamily, ResolutionFraction,
				/* AllowPostProcessSettingsScreenPercentage = */  false));
		}
	}
}
#endif

TOptional<EDLSSQualityMode> FDLSSUpscaler::GetAutoQualityModeFromViewFamily(const FSceneViewFamily& ViewFamily) const
{
	if (ensure(ViewFamily.RenderTarget != nullptr))
	{
		FIntPoint ViewSize = ViewFamily.RenderTarget->GetSizeXY();
		int32 Pixels = ViewSize.X * ViewSize.Y;
		return GetAutoQualityModeFromPixels(Pixels);
	}

	return TOptional<EDLSSQualityMode> {};
}

TOptional<EDLSSQualityMode> FDLSSUpscaler::GetAutoQualityModeFromPixels(int PixelCount) const
{
	if (PixelCount >= 8'300'000 && IsQualityModeSupported(EDLSSQualityMode::UltraPerformance))
	{
		return EDLSSQualityMode::UltraPerformance;
	}
	else if (PixelCount >= 3'690'000 && IsQualityModeSupported(EDLSSQualityMode::Performance))
	{
		return EDLSSQualityMode::Performance;
	}
	else if (PixelCount >= 2'030'000 && IsQualityModeSupported(EDLSSQualityMode::Quality))
	{
		return EDLSSQualityMode::Quality;
	}

	return TOptional<EDLSSQualityMode> {};
}


bool  FDLSSUpscaler::EnableDLSSInPlayInEditorViewports() const
{
	if (GetDefault<UDLSSOverrideSettings>()->EnableDLSSInPlayInEditorViewportsOverride == EDLSSSettingOverride::UseProjectSettings)
	{
		return GetDefault<UDLSSSettings>()->bEnableDLSSInPlayInEditorViewports;
	}
	else
	{
		return GetDefault<UDLSSOverrideSettings>()->EnableDLSSInPlayInEditorViewportsOverride == EDLSSSettingOverride::Enabled;
	}
}

float FDLSSUpscaler::GetMinUpsampleResolutionFraction() const
{
	return MinResolutionFraction;
}

float FDLSSUpscaler::GetMaxUpsampleResolutionFraction() const
{
	return MaxResolutionFraction;
}

float FDLSSUpscaler::GetOptimalResolutionFractionForQuality(EDLSSQualityMode Quality) const
{
	checkf(IsQualityModeSupported(Quality),TEXT("%u is not a valid Quality mode"), Quality);
	return ResolutionSettings[ToNGXQuality(Quality)].OptimalResolutionFraction;
}

float  FDLSSUpscaler::GetOptimalSharpnessForQuality(EDLSSQualityMode Quality) const
{
	checkf(IsQualityModeSupported(Quality), TEXT("%u is not a valid Quality mode"), Quality);
	return ResolutionSettings[ToNGXQuality(Quality)].Sharpness;
}

float FDLSSUpscaler::GetMinResolutionFractionForQuality(EDLSSQualityMode Quality) const
{
	checkf(IsQualityModeSupported(Quality), TEXT("%u is not a valid Quality mode"), Quality);
	return ResolutionSettings[ToNGXQuality(Quality)].MinResolutionFraction;
}

float FDLSSUpscaler::GetMaxResolutionFractionForQuality(EDLSSQualityMode Quality) const
{
	checkf(IsQualityModeSupported(Quality), TEXT("%u is not a valid Quality mode"), Quality);
	return ResolutionSettings[ToNGXQuality(Quality)].MaxResolutionFraction;
}

bool FDLSSUpscaler::IsFixedResolutionFraction(EDLSSQualityMode Quality) const
{
	checkf(IsQualityModeSupported(Quality), TEXT("%u is not a valid Quality mode"), Quality);
	return ResolutionSettings[ToNGXQuality(Quality)].IsFixedResolution();
}

#undef LOCTEXT_NAMESPACE
