#pragma once
#include "App.g.h"
#include "AppSettings.h"
#include "Event.h"
#include <winrt/Windows.Globalization.NumberFormatting.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>

namespace GMagpie {
class MainWindow;
}

namespace winrt::GMagpie::implementation {

struct RootPage;

class App : public App_base<App, Markup::IXamlMetadataProvider> {
public:
	static App& Get();

	App();
	App(const App&) = delete;
	App(App&&) = delete;

	bool Initialize(const wchar_t* arguments);

	int Run();

	const DispatcherQueue& Dispatcher() const noexcept {
		return _dispatcher;
	}

	void ShowMainWindow() noexcept;

	void Quit();

	void Restart(bool asElevated = false, const wchar_t* arguments = nullptr) noexcept;

	void RestartForSmoothMotion() noexcept;

	const com_ptr<RootPage>& RootPage() const noexcept;

	const ::GMagpie::MainWindow& MainWindow() const noexcept {
		return *_mainWindow;
	}

	::GMagpie::MainWindow& MainWindow() noexcept {
		return *_mainWindow;
	}

	// Запуск с --gallery — «лёгкий» экземпляр: без глобальных горячих клавиш,
	// проверки обновлений и следящего заforeground таймера, в собственной группе
	// единственного экземпляра. Такой показ уживается с основным GMagpie: не перехватывает
	// чужие хоткеи, не масштабирует чужие окна и не вытесняет запущенный экземпляр.
	bool IsGalleryLiteMode() const noexcept {
		return _galleryLiteMode;
	}

	bool IsLightTheme() const noexcept {
		return _isLightTheme.load(std::memory_order_relaxed);
	}

	static Windows::Globalization::NumberFormatting::INumberFormatter2 DoubleFormatter();
	static Windows::Globalization::NumberFormatting::INumberFormatter2 IntegerFormatter();

	::GMagpie::MultithreadEvent<bool> ThemeChanged;

private:
	void _Uninitialize();

	bool _CheckSingleInstance() noexcept;

	void _AppSettings_ThemeChanged(::GMagpie::AppTheme theme);

	void _UpdateColorValuesChangedRevoker();

	void _UpdateTheme();

	void _ReleaseMutexes() noexcept;

	wil::unique_mutex_nothrow _hSingleInstanceMutex;
	// 以管理员身份运行时持有此锁
	wil::unique_mutex_nothrow _hElevatedMutex;
	// Устанавливается до проверки единственного экземпляра по наличию --gallery
	bool _galleryLiteMode = false;

	Hosting::WindowsXamlManager _windowsXamlManager{ nullptr };

	std::unique_ptr<::GMagpie::MainWindow> _mainWindow;

	DispatcherQueue _dispatcher{ nullptr };

	::GMagpie::Event<::GMagpie::AppTheme>::EventRevoker _themeChangedRevoker;
	Windows::UI::ViewManagement::UISettings _uiSettings;
	Windows::UI::ViewManagement::UISettings::ColorValuesChanged_revoker _colorValuesChangedRevoker;
	std::atomic<bool> _isLightTheme = true;

	::GMagpie::Event<bool>::EventRevoker _isShowNotifyIconChangedRevoker;

	////////////////////////////////////////////////////
	// 
	// IXamlMetadataProvider 相关
	// 
	/////////////////////////////////////////////////////
public:
	Markup::IXamlType GetXamlType(Interop::TypeName const& type) {
		return _AppProvider()->GetXamlType(type);
	}

	Markup::IXamlType GetXamlType(hstring const& fullName) {
		return _AppProvider()->GetXamlType(fullName);
	}

	com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() {
		return _AppProvider()->GetXmlnsDefinitions();
	}

private:
	com_ptr<XamlMetaDataProvider> _AppProvider() {
		if (!_appProvider) {
			_appProvider = make_self<XamlMetaDataProvider>();
		}
		return _appProvider;
	}

	com_ptr<XamlMetaDataProvider> _appProvider;
};

}

BASIC_FACTORY(App)
