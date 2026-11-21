#pragma once
#include "ShortcutDialog.g.h"
#include "ShortcutHelper.h"

namespace winrt::GMagpie::implementation {

struct ShortcutDialog : ShortcutDialogT<ShortcutDialog> {
	void Error(::GMagpie::ShortcutError value);
	void Keys(IVector<IInspectable> value);
};

}
