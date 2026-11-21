#include "pch.h"
#include "IsNullStateTrigger.h"
#if __has_include("IsNullStateTrigger.g.cpp")
#include "IsNullStateTrigger.g.cpp"
#endif

namespace winrt::GMagpie::implementation {

DependencyProperty IsNullStateTrigger::_valueProperty{ nullptr };

IsNullStateTrigger::IsNullStateTrigger() {
	_UpdateTrigger();
}

void IsNullStateTrigger::RegisterDependencyProperties() {
	_valueProperty = DependencyProperty::Register(
		L"Value",
		xaml_typename<IInspectable>(),
		xaml_typename<GMagpie::IsNullStateTrigger>(),
		PropertyMetadata(nullptr, &IsNullStateTrigger::_OnValueChanged)
	);
}

void IsNullStateTrigger::_OnValueChanged(DependencyObject const& sender, DependencyPropertyChangedEventArgs const&) {
	get_self<IsNullStateTrigger>(sender.try_as<GMagpie::IsNullStateTrigger>())->_UpdateTrigger();
}

void IsNullStateTrigger::_UpdateTrigger() {
	SetActive(!Value());
}

}
