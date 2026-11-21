#pragma once
#include "SettingsPage.g.h"
#include "SettingsViewModel.h"

namespace winrt::GMagpie::implementation {

struct SettingsPage : SettingsPageT<SettingsPage> {
	void InitializeComponent();

	winrt::GMagpie::SettingsViewModel ViewModel() const noexcept {
		return *_viewModel;
	}

	void ComboBox_DropDownOpened(IInspectable const& sender, IInspectable const&) const;

private:
	com_ptr<SettingsViewModel> _viewModel = make_self<SettingsViewModel>();
};

}

BASIC_FACTORY(SettingsPage)
