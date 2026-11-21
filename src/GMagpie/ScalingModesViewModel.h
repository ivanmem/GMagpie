#pragma once
#include "ScalingModesViewModel.g.h"
#include "Event.h"
#include "ScalingModesService.h"

namespace winrt::GMagpie::implementation {

struct ScalingModesViewModel : ScalingModesViewModelT<ScalingModesViewModel>,
                               wil::notify_property_changed_base<ScalingModesViewModel> {
	ScalingModesViewModel();

	fire_and_forget Export() noexcept;

	fire_and_forget Import();

	IObservableVector<IInspectable> ScalingModes() const noexcept {
		return _scalingModes;
	}

	bool CanReorderScalingModes() const noexcept;
	bool HasDuplicateNames() const noexcept;

	void AddScalingMode();

private:
	fire_and_forget _AddScalingModes(
		bool isInitialExpanded = false,
		bool shouldAutoRename = false);

	void _ScalingModesService_Added(::GMagpie::EffectAddedWay way);

	void _ScalingModesService_Moved(uint32_t fromIndex, uint32_t toIndex);

	void _ScalingModesService_Removed(uint32_t index);
	void _ScalingModesService_Removing(uint32_t index);

	void _ScalingModesService_Reset();

	void _ScalingModes_VectorChanged(
		IObservableVector<IInspectable> const&,
		IVectorChangedEventArgs const& args);

	IObservableVector<IInspectable> _scalingModes = single_threaded_observable_vector<IInspectable>();

	::GMagpie::Event<::GMagpie::EffectAddedWay>::EventRevoker _scalingModeAddedRevoker;
	::GMagpie::Event<uint32_t, uint32_t>::EventRevoker _scalingModeMovedRevoker;
	::GMagpie::Event<uint32_t>::EventRevoker _scalingModeRemovedRevoker;
	::GMagpie::Event<uint32_t>::EventRevoker _scalingModeRemovingRevoker;
	::GMagpie::Event<>::EventRevoker _scalingModeNamesChangedRevoker;
	::GMagpie::Event<>::EventRevoker _scalingModesResetRevoker;
	IObservableVector<IInspectable>::VectorChanged_revoker _scalingModesChangedRevoker;

	bool _addingScalingModes = false;
	bool _pendingInitialExpanded = false;
	bool _pendingAutoRename = false;
	bool _updatingScalingModes = false;
	bool _handlingUserReorder = false;
	uint32_t _movingFromIdx = std::numeric_limits<uint32_t>::max();
	uint32_t _collectionGeneration = 0;
};

}
