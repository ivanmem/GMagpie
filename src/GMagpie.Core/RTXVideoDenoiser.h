#pragma once
#include "NativeEffectBackend.h"
#include "ScalingOptions.h"

namespace GMagpie {

class DeviceResources;

enum class RtxVideoEffectKind : uint8_t { Vsr, Denoise };

// NVIDIA VideoSuperRes modes 8-11 perform same-resolution denoising.
// HDR uses the explicit CPU-U8 bridge; SDR uses D3D11/CUDA interop.
class RTXVideoDenoiser final : public NativeEffectBackend {
public:
	RTXVideoDenoiser();
	RTXVideoDenoiser(const RTXVideoDenoiser&) = delete;
	RTXVideoDenoiser& operator=(const RTXVideoDenoiser&) = delete;
	~RTXVideoDenoiser() override;

	bool Initialize(
		DeviceResources& deviceResources,
		ID3D11Texture2D* input,
		ID3D11Texture2D* output,
		uint32_t qualityLevel,
		RtxVideoEffectKind kind
	) noexcept;

	bool Resize(
		DeviceResources& deviceResources,
		ID3D11Texture2D* input,
		ID3D11Texture2D* output
	) noexcept override;

	bool Draw(const NativeEffectDrawContext& context) noexcept override;
	EffectParameterApplyMode GetParameterApplyMode(std::string_view name) const noexcept override;
	bool ApplyParameters(const EffectOption& option,
		std::span<const std::string> names) noexcept override;
	ScalingError InitializationError() const noexcept { return _initializationError; }

	// Фото-сессия: заранее загрузить модель под размеры следующей страницы (NvVFX_Load ~2 с на каждый
	// новый размер). Вызывается с фонового потока; модель ложится в кэш, Initialize заберёт её готовой.
	// Пока ни одна модель не инициализирована обычным путём (рантайм не загружен) - ничего не делает.
	static void Preload(uint32_t inputWidth, uint32_t inputHeight,
		uint32_t outputWidth, uint32_t outputHeight, uint32_t qualityLevel) noexcept;

private:
	// В фото-сессии модель не уничтожается, а возвращается в кэш - следующая страница того же размера
	// получит её без повторной загрузки
	void _ReleaseModel() noexcept;

public:
	// Открыт только ради кэша моделей в RTXVideoDenoiser.cpp; снаружи тип неполный
	struct Impl;

private:
	[[maybe_unused]] std::unique_ptr<Impl> _impl;
	[[maybe_unused]] uint32_t _qualityLevel = 8;
	RtxVideoEffectKind _kind = RtxVideoEffectKind::Vsr;
	ScalingError _initializationError = ScalingError::NoError;
};

}
