#include "pch.h"
#include "AudioPlaybackConnector.h"

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupFlyout();
void SetupMenu();
winrt::fire_and_forget ConnectDevice(DevicePicker, std::wstring_view);
void SetupDevicePicker();
void SetupSvgIcon();
void UpdateNotifyIcon();

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
constexpr auto CONNECTION_STATE_SETTLE_DELAY = std::chrono::milliseconds(150);

bool IsCurrentConnection(std::wstring_view deviceId, uint64_t generation)
{
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

void CloseCurrentConnection(std::wstring_view deviceId, uint64_t generation, bool resetDisplayStatus)
{
	auto it = g_audioPlaybackConnections.find(std::wstring(deviceId));
	if (it == g_audioPlaybackConnections.end() || it->second.Generation != generation)
		return;

	auto device = it->second.Device;
	it->second.Connection.Close();
	g_audioPlaybackConnections.erase(it);
	if (resetDisplayStatus)
		g_devicePicker.SetDisplayStatus(device, {}, DevicePickerDisplayStatusOptions::None);
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
		if (g_reconnect)
			SaveSettings();
		for (const auto& connection : g_audioPlaybackConnections)
		{
			connection.second.Connection.Close();
			g_devicePicker.SetDisplayStatus(connection.second.Device, {}, DevicePickerDisplayStatusOptions::None);
		}
		g_audioPlaybackConnections.clear();
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

			SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_HIDEWINDOW);
			SetForegroundWindow(hWnd);
			g_devicePicker.Show(rect, Placement::Above);
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

		auto it = g_audioPlaybackConnections.find(stateChanged->deviceId);
		if (it != g_audioPlaybackConnections.end() &&
			it->second.Generation == stateChanged->generation &&
			it->second.Connection.State() == AudioPlaybackConnectionState::Closed)
		{
			auto device = it->second.Device;
			g_audioPlaybackConnections.erase(it);
			g_devicePicker.SetDisplayStatus(device, {}, DevicePickerDisplayStatusOptions::None);
		}
	}
	break;
	case WM_CONNECTDEVICE:
		if (g_reconnect)
		{
			for (const auto& i : g_lastDevices)
			{
				ConnectDevice(g_devicePicker, i);
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
		if (g_audioPlaybackConnections.size() == 0)
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

winrt::fire_and_forget ConnectDevice(DevicePicker picker, DeviceInformation device)
{
	const auto deviceId = std::wstring(device.Id());
	auto existing = g_audioPlaybackConnections.find(deviceId);
	if (existing != g_audioPlaybackConnections.end())
	{
		if (existing->second.Connecting)
			co_return;
		if (existing->second.Connection.State() == AudioPlaybackConnectionState::Opened)
		{
			picker.SetDisplayStatus(device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
			co_return;
		}
		CloseCurrentConnection(deviceId, existing->second.Generation, false);
	}

	picker.SetDisplayStatus(device, _(L"Connecting"), DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);

	bool success = false;
	uint64_t successfulGeneration = 0;
	std::wstring errorMessage;

	for (int attempt = 0; attempt < MAX_CONNECTION_ATTEMPTS && !g_shuttingDown; ++attempt)
	{
		if (attempt != 0)
			co_await winrt::resume_after(CONNECTION_RETRY_DELAY);

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
			g_audioPlaybackConnections.insert_or_assign(deviceId, AudioPlaybackConnectionEntry{
				device, connection, generation, true
			});

			connection.StateChanged([deviceId, generation](const auto& sender, const auto&) {
				if (sender.State() == AudioPlaybackConnectionState::Closed)
					QueueConnectionStateChanged(deviceId, generation);
			});

			// StartAsync configures the system-wide remote audio source. Calling it
			// repeatedly is known to be unsafe on some Windows 11 builds, so serialize
			// the first in-flight operation and never start it again in this process.
			if (!g_audioPlaybackStarted)
			{
				while (g_audioPlaybackStartInProgress && !g_audioPlaybackStarted && !g_shuttingDown)
					co_await winrt::resume_after(std::chrono::milliseconds(50));
				if (g_shuttingDown)
					co_return;

				if (!g_audioPlaybackStarted)
				{
					g_audioPlaybackStartInProgress = true;
					try
					{
						co_await connection.StartAsync();
						g_audioPlaybackStarted = true;
					}
					catch (...)
					{
						g_audioPlaybackStartInProgress = false;
						throw;
					}
					g_audioPlaybackStartInProgress = false;
				}
			}
			auto result = co_await connection.OpenAsync();

			switch (result.Status())
			{
			case AudioPlaybackConnectionOpenResultStatus::Success:
				// Win11 can report a successful open before the audio endpoint has
				// finished entering the Opened state. Do not expose a false success.
				co_await winrt::resume_after(CONNECTION_STATE_SETTLE_DELAY);
				success = IsCurrentConnection(deviceId, generation) &&
					connection.State() == AudioPlaybackConnectionState::Opened;
				if (!success)
					errorMessage = _(L"Unknown error");
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
				LOG_HR(result.ExtendedError());
				errorMessage = _(L"Unknown error");
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

		if (!IsCurrentConnection(deviceId, generation))
			co_return;
		CloseCurrentConnection(deviceId, generation, false);
	}

	if (success && IsCurrentConnection(deviceId, successfulGeneration))
	{
		auto it = g_audioPlaybackConnections.find(deviceId);
		if (it == g_audioPlaybackConnections.end())
			co_return;
		auto& entry = it->second;
		entry.Connecting = false;
		picker.SetDisplayStatus(device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
	}
	else if (!g_shuttingDown)
		picker.SetDisplayStatus(device, errorMessage.empty() ? _(L"Unknown error") : errorMessage, DevicePickerDisplayStatusOptions::ShowRetryButton);
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, std::wstring_view deviceId)
{
	try
	{
		auto device = co_await DeviceInformation::CreateFromIdAsync(deviceId);
		if (device)
			ConnectDevice(picker, device);
	}
	catch (...)
	{
		LOG_CAUGHT_EXCEPTION();
	}
}

void SetupDevicePicker()
{
	g_devicePicker = DevicePicker();
	winrt::check_hresult(g_devicePicker.as<IInitializeWithWindow>()->Initialize(g_hWnd));

	g_devicePicker.Filter().SupportedDeviceSelectors().Append(AudioPlaybackConnection::GetDeviceSelector());
	g_devicePicker.DevicePickerDismissed([](const auto&, const auto&) {
		SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_HIDEWINDOW);
	});
	g_devicePicker.DeviceSelected([](const auto& sender, const auto& args) {
		ConnectDevice(sender, args.SelectedDevice());
	});
	g_devicePicker.DisconnectButtonClicked([](const auto& sender, const auto& args) {
		auto device = args.Device();
		auto it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
		if (it != g_audioPlaybackConnections.end())
		{
			it->second.Connection.Close();
			g_audioPlaybackConnections.erase(it);
		}
		sender.SetDisplayStatus(device, {}, DevicePickerDisplayStatusOptions::None);
	});
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
