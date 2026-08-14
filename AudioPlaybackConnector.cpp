#include "pch.h"
#include "AudioPlaybackConnector.h"

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupFlyout();
void SetupMenu();
winrt::fire_and_forget ConnectDevice(std::wstring);
winrt::fire_and_forget ConnectDevice(DeviceInformation);
void RefreshDevicePicker();
void DisconnectDevice(std::wstring_view);
void SetupDevicePicker();
void SetupSvgIcon();
void UpdateNotifyIcon();

void QueueDeviceListRefresh()
{
	if (!g_shuttingDown && IsWindow(g_hWnd))
		PostMessageW(g_hWnd, WM_DEVICE_LIST_CHANGED, 0, 0);
}

bool IsLightTheme()
{
	DWORD value = 1;
	DWORD cbValue = sizeof(value);
	RegGetValueW(HKEY_CURRENT_USER,
		LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)",
		L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &cbValue);
	return value != 0;
}

winrt::Windows::UI::Color MakeColor(uint8_t alpha, uint8_t red, uint8_t green, uint8_t blue)
{
	return { alpha, red, green, blue };
}

winrt::Windows::UI::Xaml::Media::Brush CreateWin11SurfaceBrush(bool lightTheme)
{
	using namespace winrt::Windows::UI::Xaml::Media;

	AcrylicBrush brush;
	brush.BackgroundSource(AcrylicBackgroundSource::HostBackdrop);
	brush.TintColor(lightTheme ? MakeColor(255, 250, 250, 250) : MakeColor(255, 32, 32, 32));
	brush.TintOpacity(lightTheme ? 0.92 : 0.86);
	brush.FallbackColor(lightTheme ? MakeColor(255, 250, 250, 250) : MakeColor(255, 32, 32, 32));
	return brush;
}

winrt::Windows::UI::Xaml::Media::Brush CreateTextBrush(bool lightTheme, uint8_t alpha = 255)
{
	using namespace winrt::Windows::UI::Xaml::Media;
	return SolidColorBrush(lightTheme ? MakeColor(alpha, 32, 32, 32) : MakeColor(alpha, 255, 255, 255));
}

void ApplyWin11MenuStyle(MenuFlyout& menu, bool lightTheme)
{
	using namespace winrt::Windows::UI::Xaml;
	using namespace winrt::Windows::UI::Xaml::Controls;

	Style presenterStyle;
	presenterStyle.TargetType(winrt::xaml_typename<MenuFlyoutPresenter>());
	presenterStyle.Setters().Append(Setter(Control::BackgroundProperty(),
		winrt::box_value(CreateWin11SurfaceBrush(lightTheme))));
	presenterStyle.Setters().Append(Setter(Control::PaddingProperty(),
		winrt::box_value(Thickness{ 4, 6, 4, 6 })));
	presenterStyle.Setters().Append(Setter(Control::CornerRadiusProperty(),
		winrt::box_value(CornerRadius{ 10, 10, 10, 10 })));
	menu.MenuFlyoutPresenterStyle(presenterStyle);
}

constexpr int MAX_CONNECTION_ATTEMPTS = 3;
constexpr auto CONNECTION_RETRY_DELAY = std::chrono::milliseconds(400);
constexpr auto CONNECTION_OPERATION_POLL_INTERVAL = std::chrono::milliseconds(100);
constexpr auto CONNECTION_START_TIMEOUT = std::chrono::seconds(10);
constexpr auto CONNECTION_OPEN_TIMEOUT = std::chrono::seconds(20);
constexpr auto INITIAL_AUDIO_SINK_WARMUP_DELAY = std::chrono::milliseconds(1200);

bool IsCurrentConnection(std::wstring_view deviceId, uint64_t generation)
{
	std::lock_guard lock(g_connectionMutex);
	auto it = g_audioPlaybackConnections.find(std::wstring(deviceId));
	return it != g_audioPlaybackConnections.end() && it->second.Generation == generation;
}

void QueueConnectionStateChanged(std::wstring deviceId, uint64_t generation)
{
	// This callback may run on a non-UI thread. Only post a message here;
	// the connection table is intentionally touched by the UI thread alone.
	if (g_shuttingDown || !IsWindow(g_hWnd))
		return;

	auto message = std::make_unique<ConnectionStateChangedMessage>();
	message->deviceId = std::move(deviceId);
	message->generation = generation;
	if (PostMessageW(g_hWnd, WM_CONNECTION_STATE_CHANGED, 0, reinterpret_cast<LPARAM>(message.get())))
		message.release();
}

void CloseCurrentConnection(std::wstring_view deviceId, uint64_t generation)
{
	std::lock_guard lock(g_connectionMutex);
	auto it = g_audioPlaybackConnections.find(std::wstring(deviceId));
	if (it == g_audioPlaybackConnections.end() || it->second.Generation != generation)
		return;

	it->second.Connection.Close();
	g_audioPlaybackConnections.erase(it);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	g_hInst = hInstance;

	winrt::init_apartment();

	bool supported = false;
	try
	{
		using namespace winrt::Windows::Foundation::Metadata;

		supported = ApiInformation::IsTypePresent(winrt::name_of<DesktopWindowXamlSource>()) &&
			ApiInformation::IsTypePresent(winrt::name_of<AudioPlaybackConnection>());
	}
	catch (winrt::hresult_error const&)
	{
		supported = false;
		LOG_CAUGHT_EXCEPTION();
	}
	if (!supported)
	{
		TaskDialog(nullptr, nullptr, _(L"Unsupported Operating System"), nullptr, _(L"AudioPlaybackConnector is not supported on this operating system version."), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		return EXIT_FAILURE;
	}

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_AUDIOPLAYBACKCONNECTOR)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = L"AudioPlaybackConnector",
		.hIconSm = wcex.hIcon
	};

	RegisterClassExW(&wcex);

	// When parent window size is 0x0 or invisible, the dpi scale of menu is incorrect. Here we set window size to 1x1 and use WS_EX_LAYERED to make window looks like invisible.
	g_hWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST, L"AudioPlaybackConnector", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hWnd);
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA));

	DesktopWindowXamlSource desktopSource;
	auto desktopSourceNative2 = desktopSource.as<IDesktopWindowXamlSourceNative2>();
	winrt::check_hresult(desktopSourceNative2->AttachToWindow(g_hWnd));
	winrt::check_hresult(desktopSourceNative2->get_WindowHandle(&g_hWndXaml));

	g_xamlCanvas = Canvas();
	desktopSource.Content(g_xamlCanvas);

	LoadSettings();
	SetupFlyout();
	SetupMenu();
	SetupDevicePicker();
	SetupSvgIcon();

	g_nid.hWnd = g_niid.hWnd = g_hWnd;
	wcscpy_s(g_nid.szTip, _(L"AudioPlaybackConnector"));
	UpdateNotifyIcon();

	WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(WM_TASKBAR_CREATED == 0);

	PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		BOOL processed = FALSE;
		winrt::check_hresult(desktopSourceNative2->PreTranslateMessage(&msg, &processed));
		if (!processed)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_DESTROY:
		g_shuttingDown = true;
		if (g_deviceWatcher)
			g_deviceWatcher.Stop();
		if (g_reconnect)
			SaveSettings();
		{
			std::lock_guard lock(g_connectionMutex);
			for (const auto& connection : g_audioPlaybackConnections)
				connection.second.Connection.Close();
			g_audioPlaybackConnections.clear();
			g_deviceErrorMessages.clear();
		}
		if (!g_reconnect)
		{
			SaveSettings();
		}
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		PostQuitMessage(0);
		break;
	case WM_SETTINGCHANGE:
		if (lParam && CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)
		{
			UpdateNotifyIcon();
			SetupFlyout();
			SetupMenu();
			SetupDevicePicker();
		}
		break;
	case WM_NOTIFYICON:
		switch (LOWORD(lParam))
		{
		case NIN_SELECT:
		case NIN_KEYSELECT:
		{
			using namespace winrt::Windows::UI::Popups;

			RECT iconRect;
			auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
			if (FAILED(hr))
			{
				LOG_HR(hr);
				break;
			}

			auto dpi = GetDpiForWindow(hWnd);
			Rect rect = {
				static_cast<float>(iconRect.left * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(iconRect.top * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(g_hWndXaml, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
			SetWindowPos(hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 1, 1, SWP_SHOWWINDOW);
			SetForegroundWindow(hWnd);
			g_xamlCanvas.Width(rect.Width);
			g_xamlCanvas.Height(rect.Height);
			g_devicePickerVisible = true;
			RefreshDevicePicker();
			g_xamlDeviceFlyout.ShowAt(g_xamlCanvas);
		}
		break;
		case WM_RBUTTONUP: // Menu activated by mouse click
			g_menuFocusState = FocusState::Pointer;
			break;
		case WM_CONTEXTMENU:
		{
			if (g_menuFocusState == FocusState::Unfocused)
				g_menuFocusState = FocusState::Keyboard;

			auto dpi = GetDpiForWindow(hWnd);
			Point point = {
				static_cast<float>(GET_X_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(GET_Y_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(g_hWndXaml, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
			SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
			SetForegroundWindow(hWnd);

			g_xamlMenu.ShowAt(g_xamlCanvas, point);
		}
		break;
		}
		break;
	case WM_CONNECTION_STATE_CHANGED:
	{
		std::unique_ptr<ConnectionStateChangedMessage> stateChanged(reinterpret_cast<ConnectionStateChangedMessage*>(lParam));
		if (!stateChanged || g_shuttingDown)
			break;

		{
			std::lock_guard lock(g_connectionMutex);
			auto it = g_audioPlaybackConnections.find(stateChanged->deviceId);
			if (it == g_audioPlaybackConnections.end() ||
				it->second.Generation != stateChanged->generation ||
				it->second.Connection.State() != AudioPlaybackConnectionState::Closed)
				break;
			g_audioPlaybackConnections.erase(it);
			QueueDeviceListRefresh();
		}
	}
	break;
	case WM_DEVICE_LIST_CHANGED:
		RefreshDevicePicker();
		break;
	case WM_CONNECTDEVICE:
		if (g_reconnect)
		{
			for (const auto& i : g_lastDevices)
			{
				ConnectDevice(i);
			}
			g_lastDevices.clear();
		}
		break;
	default:
		if (WM_TASKBAR_CREATED && message == WM_TASKBAR_CREATED)
		{
			UpdateNotifyIcon();
		}
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}
	return 0;
}

void SetupFlyout()
{
	using namespace winrt::Windows::UI::Xaml::Media;

	const bool lightTheme = IsLightTheme();
	const auto textBrush = CreateTextBrush(lightTheme);
	const auto secondaryTextBrush = CreateTextBrush(lightTheme, 190);

	FontIcon exitIcon;
	exitIcon.Glyph(L"\xE8BB");
	exitIcon.FontSize(22);
	exitIcon.Foreground(SolidColorBrush(MakeColor(255, 0, 103, 192)));
	exitIcon.Margin({ 0, 0, 12, 0 });

	TextBlock title;
	title.Text(_(L"Exit"));
	title.FontSize(20);
	title.FontWeight({ 600 });
	title.Foreground(textBrush);

	TextBlock textBlock;
	textBlock.Text(_(L"All connections will be closed.\nExit anyway?"));
	textBlock.FontSize(14);
	textBlock.TextWrapping(TextWrapping::Wrap);
	textBlock.Foreground(secondaryTextBrush);
	textBlock.Margin({ 0, 8, 0, 18 });

	static CheckBox checkbox;
	checkbox.IsChecked(g_reconnect);
	checkbox.Content(winrt::box_value(_(L"Reconnect on next start")));
	checkbox.FontSize(14);
	checkbox.Foreground(textBrush);
	checkbox.Margin({ 0, 0, 0, 18 });

	Button button;
	button.Content(winrt::box_value(_(L"Exit")));
	button.HorizontalAlignment(HorizontalAlignment::Right);
	button.FontSize(14);
	button.FontWeight({ 600 });
	button.Padding({ 18, 8, 18, 8 });
	button.CornerRadius({ 6, 6, 6, 6 });
	button.Background(SolidColorBrush(MakeColor(255, 0, 103, 192)));
	button.Foreground(SolidColorBrush(MakeColor(255, 255, 255, 255)));
	button.Click([](const auto&, const auto&) {
		g_reconnect = checkbox.IsChecked().Value();
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
	});

	StackPanel titlePanel;
	titlePanel.Orientation(Orientation::Horizontal);
	titlePanel.VerticalAlignment(VerticalAlignment::Center);
	titlePanel.Children().Append(exitIcon);
	titlePanel.Children().Append(title);

	StackPanel stackPanel;
	stackPanel.Width(320);
	stackPanel.Spacing(0);
	stackPanel.Padding({ 20, 18, 20, 18 });
	stackPanel.Children().Append(titlePanel);
	stackPanel.Children().Append(textBlock);
	stackPanel.Children().Append(checkbox);
	stackPanel.Children().Append(button);

	Border card;
	card.Background(CreateWin11SurfaceBrush(lightTheme));
	card.CornerRadius({ 12, 12, 12, 12 });
	card.BorderBrush(SolidColorBrush(lightTheme ? MakeColor(90, 255, 255, 255) : MakeColor(90, 255, 255, 255)));
	card.BorderThickness({ 1, 1, 1, 1 });
	card.Shadow(ThemeShadow());
	card.Child(stackPanel);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(card);

	g_xamlFlyout = flyout;
}

void SetupMenu()
{
	const bool lightTheme = IsLightTheme();

	// https://docs.microsoft.com/en-us/windows/uwp/design/style/segoe-ui-symbol-font
	FontIcon settingsIcon;
	settingsIcon.Glyph(L"\xE713");

	MenuFlyoutItem settingsItem;
	settingsItem.Text(_(L"Bluetooth Settings"));
	settingsItem.Icon(settingsIcon);
	settingsItem.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	FontIcon closeIcon;
	closeIcon.Glyph(L"\xE8BB");

	MenuFlyoutItem exitItem;
	exitItem.Text(_(L"Exit"));
	exitItem.Icon(closeIcon);
	exitItem.Click([](const auto&, const auto&) {
		bool hasConnections = false;
		{
			std::lock_guard lock(g_connectionMutex);
			hasConnections = !g_audioPlaybackConnections.empty();
		}
		if (!hasConnections)
		{
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
			return;
		}

		RECT iconRect;
		auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
		if (FAILED(hr))
		{
			LOG_HR(hr);
			return;
		}

		auto dpi = GetDpiForWindow(g_hWnd);

		SetWindowPos(g_hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 0, 0, SWP_HIDEWINDOW);
		g_xamlCanvas.Width(static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi));
		g_xamlCanvas.Height(static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi));

		g_xamlFlyout.ShowAt(g_xamlCanvas);
	});

	MenuFlyout menu;
	menu.Items().Append(settingsItem);
	menu.Items().Append(exitItem);
	ApplyWin11MenuStyle(menu, lightTheme);
	menu.Opened([](const auto& sender, const auto&) {
		auto menuItems = sender.as<MenuFlyout>().Items();
		auto itemsCount = menuItems.Size();
		if (itemsCount > 0)
		{
			menuItems.GetAt(itemsCount - 1).Focus(g_menuFocusState);
		}
		g_menuFocusState = FocusState::Unfocused;
	});
	menu.Closed([](const auto&, const auto&) {
		ShowWindow(g_hWnd, SW_HIDE);
	});

	g_xamlMenu = menu;
}

winrt::fire_and_forget ConnectDevice(std::wstring deviceId)
{
	// DeviceWatcher callbacks can run outside the XAML apartment. Recreate the
	// device on the UI apartment before starting the audio connection, matching
	// the object flow used by the original DevicePicker implementation.
	auto uiContext = winrt::apartment_context();
	try
	{
		auto device = co_await DeviceInformation::CreateFromIdAsync(deviceId);
		co_await uiContext;
		if (g_shuttingDown)
			co_return;
		if (device)
		{
			ConnectDevice(device);
			co_return;
		}
	}
	catch (winrt::hresult_error const& ex)
	{
		if (g_shuttingDown)
			co_return;

		std::wstring errorMessage = ex.message().c_str();
		errorMessage += L" (0x";
		wchar_t errorCode[9]{};
		swprintf_s(errorCode, L"%08X", static_cast<uint32_t>(ex.code()));
		errorMessage += errorCode;
		errorMessage += L")";
		{
			std::lock_guard lock(g_connectionMutex);
			g_deviceErrorMessages[deviceId] = std::move(errorMessage);
		}
		QueueDeviceListRefresh();
		LOG_CAUGHT_EXCEPTION();
		co_return;
	}
	catch (...)
	{
		if (g_shuttingDown)
			co_return;
	}

	{
		std::lock_guard lock(g_connectionMutex);
		g_deviceErrorMessages[deviceId] = _(L"Unknown error");
	}
	QueueDeviceListRefresh();
}

winrt::fire_and_forget ConnectDevice(DeviceInformation device)
{
	// The original project performed the complete StartAsync/OpenAsync sequence
	// from the picker UI apartment. Preserve that behavior while the custom UI
	// remains responsible only for displaying device rows.
	auto uiContext = winrt::apartment_context();
	const auto deviceId = std::wstring(device.Id());
	{
		std::lock_guard lock(g_connectionMutex);
		g_deviceErrorMessages.erase(deviceId);
	}
	QueueDeviceListRefresh();

	{
		std::lock_guard lock(g_connectionMutex);
		auto existing = g_audioPlaybackConnections.find(deviceId);
		if (existing != g_audioPlaybackConnections.end())
		{
			if (existing->second.Connecting)
				co_return;
			if (existing->second.Connection.State() == AudioPlaybackConnectionState::Opened)
				co_return;
		}
		if (existing != g_audioPlaybackConnections.end())
		{
			existing->second.Connection.Close();
			g_audioPlaybackConnections.erase(existing);
		}
	}

	bool success = false;
	uint64_t successfulGeneration = 0;
	std::wstring errorMessage;

	for (int attempt = 0; attempt < MAX_CONNECTION_ATTEMPTS && !g_shuttingDown; ++attempt)
	{
		if (attempt != 0)
		{
			co_await winrt::resume_after(CONNECTION_RETRY_DELAY);
			co_await uiContext;
		}

		AudioPlaybackConnection connection = nullptr;
		uint64_t generation = 0;
		try
		{
			connection = AudioPlaybackConnection::TryCreateFromId(device.Id());
			if (!connection)
			{
				errorMessage = _(L"Unknown error");
				continue;
			}

			generation = ++g_nextConnectionGeneration;
			{
				std::lock_guard lock(g_connectionMutex);
				g_audioPlaybackConnections.insert_or_assign(deviceId, AudioPlaybackConnectionEntry{
					connection, generation, true
				});
			}
			QueueDeviceListRefresh();

			connection.StateChanged([deviceId, generation](const auto& sender, const auto&) {
				if (sender.State() == AudioPlaybackConnectionState::Closed)
					QueueConnectionStateChanged(deviceId, generation);
			});

			// Keep the original enable/open order, but do not let a Windows Bluetooth
			// operation leave the custom picker permanently stuck in "Connecting".
			auto startOperation = connection.StartAsync();
			const auto startDeadline = std::chrono::steady_clock::now() + CONNECTION_START_TIMEOUT;
			while (startOperation.Status() == AsyncStatus::Started &&
				std::chrono::steady_clock::now() < startDeadline && !g_shuttingDown)
				co_await winrt::resume_after(CONNECTION_OPERATION_POLL_INTERVAL);
			co_await uiContext;
			if (g_shuttingDown)
			{
				startOperation.Cancel();
				co_return;
			}
			if (startOperation.Status() == AsyncStatus::Started)
			{
				startOperation.Cancel();
				winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
			}
			startOperation.GetResults();

			// On Windows 11, the first StartAsync can complete before the A2DP sink
			// endpoint is ready to route audio. Give only the initial connection a
			// short warm-up period, then return to the UI apartment before OpenAsync.
			bool warmupPending = true;
			if (g_initialAudioSinkWarmupPending.compare_exchange_strong(warmupPending, false))
			{
				co_await winrt::resume_after(INITIAL_AUDIO_SINK_WARMUP_DELAY);
				co_await uiContext;
				if (g_shuttingDown || !IsCurrentConnection(deviceId, generation))
					co_return;
			}

			auto openOperation = connection.OpenAsync();
			const auto openDeadline = std::chrono::steady_clock::now() + CONNECTION_OPEN_TIMEOUT;
			while (openOperation.Status() == AsyncStatus::Started &&
				std::chrono::steady_clock::now() < openDeadline && !g_shuttingDown)
				co_await winrt::resume_after(CONNECTION_OPERATION_POLL_INTERVAL);
			co_await uiContext;
			if (g_shuttingDown)
			{
				openOperation.Cancel();
				co_return;
			}
			if (openOperation.Status() == AsyncStatus::Started)
			{
				openOperation.Cancel();
				winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
			}
			auto result = openOperation.GetResults();

			switch (result.Status())
			{
			case AudioPlaybackConnectionOpenResultStatus::Success:
				// This is intentionally based on OpenAsync's result, like the
				// original implementation. StateChanged keeps the UI in sync later.
				success = IsCurrentConnection(deviceId, generation);
				break;
			case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
				success = false;
				errorMessage = _(L"The request timed out");
				break;
			case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
				success = false;
				errorMessage = _(L"The operation was denied by the system");
				break;
			case AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
				success = false;
				{
					const auto extendedError = result.ExtendedError();
					LOG_HR(extendedError);
					wchar_t errorCode[16]{};
					swprintf_s(errorCode, L" (0x%08X)", static_cast<uint32_t>(extendedError));
					errorMessage = _(L"Unknown error");
					errorMessage += errorCode;
				}
				break;
			}
		}
		catch (winrt::hresult_error const& ex)
		{
			success = false;
			errorMessage.resize(64);
			while (1)
			{
				auto result = swprintf(errorMessage.data(), errorMessage.size(), L"%s (0x%08X)", ex.message().c_str(), static_cast<uint32_t>(ex.code()));
				if (result < 0)
					errorMessage.resize(errorMessage.size() * 2);
				else
				{
					errorMessage.resize(result);
					break;
				}
			}
			LOG_CAUGHT_EXCEPTION();
		}
		catch (...)
		{
			success = false;
			errorMessage = _(L"Unknown error");
			LOG_CAUGHT_EXCEPTION();
		}

		if (success)
		{
			successfulGeneration = generation;
			break;
		}

		if (generation != 0)
		{
			if (!IsCurrentConnection(deviceId, generation))
				co_return;
			CloseCurrentConnection(deviceId, generation);
		}
	}

	if (success && IsCurrentConnection(deviceId, successfulGeneration))
	{
		{
			std::lock_guard lock(g_connectionMutex);
			auto it = g_audioPlaybackConnections.find(deviceId);
			if (it == g_audioPlaybackConnections.end())
				co_return;
			it->second.Connecting = false;
			g_deviceErrorMessages.erase(deviceId);
		}
	}
	else if (!g_shuttingDown)
	{
		std::lock_guard lock(g_connectionMutex);
		g_deviceErrorMessages[deviceId] = errorMessage.empty() ? _(L"Unknown error") : errorMessage;
	}

	QueueDeviceListRefresh();
}

void DisconnectDevice(std::wstring_view deviceId)
{
	{
		std::lock_guard lock(g_connectionMutex);
		auto it = g_audioPlaybackConnections.find(std::wstring(deviceId));
		if (it != g_audioPlaybackConnections.end())
		{
			it->second.Connection.Close();
			g_audioPlaybackConnections.erase(it);
		}
		g_deviceErrorMessages.erase(std::wstring(deviceId));
	}
	QueueDeviceListRefresh();
}

void AddDevicePickerRow(std::wstring const& deviceId, std::wstring const& deviceName, bool lightTheme)
{
	using namespace winrt::Windows::UI::Xaml::Media;

	bool connecting = false;
	bool connected = false;
	std::wstring errorMessage;
	{
		std::lock_guard lock(g_connectionMutex);
		auto connection = g_audioPlaybackConnections.find(deviceId);
		connecting = connection != g_audioPlaybackConnections.end() && connection->second.Connecting;
		// UI status is derived from the completed connection operation. Avoid
		// querying the WinRT audio object merely to repaint the custom picker.
		connected = connection != g_audioPlaybackConnections.end() && !connecting;
		auto error = g_deviceErrorMessages.find(deviceId);
		if (error != g_deviceErrorMessages.end())
			errorMessage = error->second;
	}

	Grid row;
	row.MinHeight(68);
	row.Padding({ 8, 6, 8, 6 });
	row.CornerRadius({ 8, 8, 8, 8 });
	row.Background(SolidColorBrush(lightTheme ? MakeColor(24, 0, 0, 0) : MakeColor(32, 255, 255, 255)));

	ColumnDefinition iconColumn;
	iconColumn.Width(GridLength{ 44, GridUnitType::Pixel });
	ColumnDefinition textColumn;
	textColumn.Width(GridLength{ 1, GridUnitType::Star });
	ColumnDefinition actionColumn;
	actionColumn.Width(GridLength{ 110, GridUnitType::Pixel });
	row.ColumnDefinitions().Append(iconColumn);
	row.ColumnDefinitions().Append(textColumn);
	row.ColumnDefinitions().Append(actionColumn);

	FontIcon deviceIcon;
	deviceIcon.Glyph(L"\xE702");
	deviceIcon.FontSize(24);
	deviceIcon.Foreground(SolidColorBrush(MakeColor(255, 0, 103, 192)));
	deviceIcon.HorizontalAlignment(HorizontalAlignment::Center);
	deviceIcon.VerticalAlignment(VerticalAlignment::Center);
	Grid::SetColumn(deviceIcon, 0);
	row.Children().Append(deviceIcon);

	StackPanel textPanel;
	textPanel.VerticalAlignment(VerticalAlignment::Center);
	textPanel.Margin({ 8, 0, 8, 0 });

	TextBlock name;
	name.Text(deviceName.empty() ? _(L"Unknown device") : deviceName);
	name.FontSize(15);
	name.Foreground(CreateTextBrush(lightTheme));
	name.TextTrimming(TextTrimming::CharacterEllipsis);

	TextBlock status;
	status.FontSize(13);
	status.Foreground(CreateTextBrush(lightTheme, 175));
	if (connecting)
		status.Text(_(L"Connecting"));
	else if (connected)
		status.Text(_(L"Connected"));
	else
	{
		status.Text(errorMessage.empty() ? _(L"Ready") : errorMessage);
	}

	textPanel.Children().Append(name);
	textPanel.Children().Append(status);
	Grid::SetColumn(textPanel, 1);
	row.Children().Append(textPanel);

	Button action;
	action.MinWidth(104);
	action.Padding({ 12, 6, 12, 6 });
	action.FontSize(13);
	action.CornerRadius({ 6, 6, 6, 6 });
	action.IsEnabled(!connecting);
	action.Content(winrt::box_value(connected ? _(L"Disconnect") : connecting ? _(L"Connecting") : _(L"Connect")));
	if (connected)
	{
		action.Background(SolidColorBrush(MakeColor(255, 0, 103, 192)));
		action.Foreground(SolidColorBrush(MakeColor(255, 255, 255, 255)));
	}
	else
	{
		action.Background(SolidColorBrush(lightTheme ? MakeColor(34, 0, 0, 0) : MakeColor(48, 255, 255, 255)));
		action.Foreground(CreateTextBrush(lightTheme));
	}
	action.Click([deviceId, connected](const auto&, const auto&) {
		if (connected)
			DisconnectDevice(deviceId);
		else
			ConnectDevice(deviceId);
	});
	Grid::SetColumn(action, 2);
	row.Children().Append(action);

	g_deviceListPanel.Children().Append(row);
}

void RefreshDevicePicker()
{
	if (g_shuttingDown || !g_deviceListPanel)
		return;

	std::vector<std::pair<std::wstring, std::wstring>> devices;
	{
		std::lock_guard lock(g_deviceListMutex);
		devices.reserve(g_availableDeviceNames.size());
		for (const auto& device : g_availableDeviceNames)
			devices.push_back(device);
	}

	const bool lightTheme = IsLightTheme();
	g_deviceListPanel.Children().Clear();
	if (devices.empty())
	{
		TextBlock empty;
		empty.Text(g_deviceEnumerationCompleted ? _(L"No compatible audio devices found") : _(L"Searching for Bluetooth audio devices..."));
		empty.FontSize(14);
		empty.Foreground(CreateTextBrush(lightTheme, 190));
		empty.TextWrapping(TextWrapping::Wrap);
		empty.Margin({ 8, 16, 8, 16 });
		g_deviceListPanel.Children().Append(empty);
	}
	else
	{
		for (const auto& [deviceId, deviceName] : devices)
			AddDevicePickerRow(deviceId, deviceName, lightTheme);
	}
}

void SetupDevicePicker()
{
	using namespace winrt::Windows::UI::Xaml::Media;

	const bool lightTheme = IsLightTheme();
	const auto watcherGeneration = ++g_deviceWatcherGeneration;
	try
	{
		if (g_deviceWatcher)
			g_deviceWatcher.Stop();

		{
			std::lock_guard lock(g_deviceListMutex);
			g_availableDeviceNames.clear();
		}
		g_deviceEnumerationCompleted = false;
		g_deviceWatcher = DeviceInformation::CreateWatcher(AudioPlaybackConnection::GetDeviceSelector());
		g_deviceWatcher.Added([watcherGeneration](const auto&, const auto& device) {
			if (g_shuttingDown || watcherGeneration != g_deviceWatcherGeneration)
				return;
			{
				std::lock_guard lock(g_deviceListMutex);
				g_availableDeviceNames.insert_or_assign(std::wstring(device.Id()), std::wstring(device.Name()));
			}
			if (g_devicePickerVisible && IsWindow(g_hWnd))
				PostMessageW(g_hWnd, WM_DEVICE_LIST_CHANGED, 0, 0);
		});
		g_deviceWatcher.Removed([watcherGeneration](const auto&, const auto& update) {
			if (g_shuttingDown || watcherGeneration != g_deviceWatcherGeneration)
				return;
			{
				std::lock_guard lock(g_deviceListMutex);
				g_availableDeviceNames.erase(std::wstring(update.Id()));
			}
			if (g_devicePickerVisible && IsWindow(g_hWnd))
				PostMessageW(g_hWnd, WM_DEVICE_LIST_CHANGED, 0, 0);
		});
		g_deviceWatcher.Updated([watcherGeneration](const auto&, const auto&) {
			if (g_shuttingDown || watcherGeneration != g_deviceWatcherGeneration)
				return;
			if (g_devicePickerVisible && IsWindow(g_hWnd))
				PostMessageW(g_hWnd, WM_DEVICE_LIST_CHANGED, 0, 0);
		});
		g_deviceWatcher.EnumerationCompleted([watcherGeneration](const auto&, const auto&) {
			if (g_shuttingDown || watcherGeneration != g_deviceWatcherGeneration)
				return;
			g_deviceEnumerationCompleted = true;
			if (g_devicePickerVisible && IsWindow(g_hWnd))
				PostMessageW(g_hWnd, WM_DEVICE_LIST_CHANGED, 0, 0);
		});
		g_deviceWatcher.Start();
	}
	catch (...)
	{
		g_deviceEnumerationCompleted = true;
		LOG_CAUGHT_EXCEPTION();
	}

	const auto textBrush = CreateTextBrush(lightTheme);
	const auto secondaryTextBrush = CreateTextBrush(lightTheme, 185);

	FontIcon bluetoothIcon;
	bluetoothIcon.Glyph(L"\xE702");
	bluetoothIcon.FontSize(24);
	bluetoothIcon.Foreground(SolidColorBrush(MakeColor(255, 0, 103, 192)));
	bluetoothIcon.Margin({ 0, 0, 10, 0 });

	TextBlock title;
	title.Text(_(L"Connect"));
	title.FontSize(20);
	title.FontWeight({ 600 });
	title.Foreground(textBrush);

	StackPanel titlePanel;
	titlePanel.Orientation(Orientation::Horizontal);
	titlePanel.VerticalAlignment(VerticalAlignment::Center);
	titlePanel.Children().Append(bluetoothIcon);
	titlePanel.Children().Append(title);

	TextBlock subtitle;
	subtitle.Text(_(L"Select a Bluetooth audio device"));
	subtitle.FontSize(13);
	subtitle.Foreground(secondaryTextBrush);
	subtitle.Margin({ 0, 4, 0, 0 });

	g_deviceListPanel = StackPanel();
	g_deviceListPanel.Spacing(6);

	ScrollViewer deviceScroll;
	deviceScroll.MaxHeight(420);
	deviceScroll.Margin({ 0, 16, 0, 16 });
	deviceScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
	deviceScroll.Content(g_deviceListPanel);

	Button settingsButton;
	settingsButton.Content(winrt::box_value(_(L"Bluetooth Settings")));
	settingsButton.FontSize(13);
	settingsButton.Padding({ 12, 7, 12, 7 });
	settingsButton.CornerRadius({ 6, 6, 6, 6 });
	settingsButton.Background(SolidColorBrush(lightTheme ? MakeColor(34, 0, 0, 0) : MakeColor(48, 255, 255, 255)));
	settingsButton.Foreground(textBrush);
	settingsButton.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	Button cancelButton;
	cancelButton.Content(winrt::box_value(_(L"Cancel")));
	cancelButton.FontSize(13);
	cancelButton.FontWeight({ 600 });
	cancelButton.Padding({ 16, 7, 16, 7 });
	cancelButton.CornerRadius({ 6, 6, 6, 6 });
	cancelButton.Background(SolidColorBrush(MakeColor(255, 0, 103, 192)));
	cancelButton.Foreground(SolidColorBrush(MakeColor(255, 255, 255, 255)));
	cancelButton.Click([](const auto&, const auto&) {
		g_xamlDeviceFlyout.Hide();
	});

	StackPanel footer;
	footer.Orientation(Orientation::Horizontal);
	footer.HorizontalAlignment(HorizontalAlignment::Right);
	footer.Spacing(8);
	footer.Children().Append(settingsButton);
	footer.Children().Append(cancelButton);

	StackPanel content;
	content.Width(420);
	content.Padding({ 20, 18, 20, 18 });
	content.Children().Append(titlePanel);
	content.Children().Append(subtitle);
	content.Children().Append(deviceScroll);
	content.Children().Append(footer);

	Border card;
	card.Background(CreateWin11SurfaceBrush(lightTheme));
	card.CornerRadius({ 12, 12, 12, 12 });
	card.BorderBrush(SolidColorBrush(lightTheme ? MakeColor(90, 255, 255, 255) : MakeColor(90, 255, 255, 255)));
	card.BorderThickness({ 1, 1, 1, 1 });
	card.Shadow(ThemeShadow());
	card.Child(content);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Placement(winrt::Windows::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Top);
	flyout.Content(card);
	flyout.Closed([](const auto&, const auto&) {
		g_devicePickerVisible = false;
		ShowWindow(g_hWnd, SW_HIDE);
	});

	g_xamlDeviceFlyout = flyout;
}

void SetupSvgIcon()
{
	auto hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(1), L"SVG");
	FAIL_FAST_LAST_ERROR_IF_NULL(hRes);

	auto size = SizeofResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF(size == 0);

	auto hResData = LoadResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF_NULL(hResData);

	auto svgData = reinterpret_cast<const char*>(LockResource(hResData));
	FAIL_FAST_IF_NULL_ALLOC(svgData);

	const std::string_view svg(svgData, size);
	const int width = GetSystemMetrics(SM_CXSMICON), height = GetSystemMetrics(SM_CYSMICON);

	g_hIconLight = SvgTohIcon(svg, width, height, { 0, 0, 0, 1 });
	g_hIconDark = SvgTohIcon(svg, width, height, { 1, 1, 1, 1 });
}

void UpdateNotifyIcon()
{
	DWORD value = 0, cbValue = sizeof(value);
	LOG_IF_WIN32_ERROR(RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &cbValue));
	g_nid.hIcon = value != 0 ? g_hIconLight : g_hIconDark;

	if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid))
	{
		if (Shell_NotifyIconW(NIM_ADD, &g_nid))
		{
			FAIL_FAST_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_SETVERSION, &g_nid));
		}
		else
		{
			LOG_LAST_ERROR();
		}
	}
}
