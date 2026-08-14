#pragma once

#include "resource.h"

using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Audio;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Hosting;
namespace fs = std::filesystem;

constexpr UINT WM_NOTIFYICON = WM_APP + 1;
constexpr UINT WM_CONNECTDEVICE = WM_APP + 2;
constexpr UINT WM_CONNECTION_STATE_CHANGED = WM_APP + 3;
constexpr UINT WM_DEVICE_LIST_CHANGED = WM_APP + 4;

struct ConnectionStateChangedMessage
{
	std::wstring deviceId;
	uint64_t generation{};
};

struct AudioPlaybackConnectionEntry
{
	AudioPlaybackConnection Connection{ nullptr };
	uint64_t Generation{};
	bool Connecting{};
};

HINSTANCE g_hInst;
HWND g_hWnd;
HWND g_hWndXaml;
Canvas g_xamlCanvas = nullptr;
Flyout g_xamlFlyout = nullptr;
Flyout g_xamlDeviceFlyout = nullptr;
StackPanel g_deviceListPanel = nullptr;
MenuFlyout g_xamlMenu = nullptr;
FocusState g_menuFocusState = FocusState::Unfocused;
DeviceWatcher g_deviceWatcher = nullptr;
std::unordered_map<std::wstring, AudioPlaybackConnectionEntry> g_audioPlaybackConnections;
std::unordered_map<std::wstring, std::wstring> g_deviceErrorMessages;
std::unordered_map<std::wstring, std::wstring> g_availableDeviceNames;
std::recursive_mutex g_connectionMutex;
std::mutex g_deviceListMutex;
HICON g_hIconLight = nullptr;
HICON g_hIconDark = nullptr;
NOTIFYICONDATAW g_nid = {
	.cbSize = sizeof(g_nid),
	.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP,
	.uCallbackMessage = WM_NOTIFYICON,
	.uVersion = NOTIFYICON_VERSION_4
};
NOTIFYICONIDENTIFIER g_niid = {
	.cbSize = sizeof(g_niid)
};
UINT WM_TASKBAR_CREATED = 0;
bool g_reconnect = false;
std::vector<std::wstring> g_lastDevices;
uint64_t g_nextConnectionGeneration = 0;
std::atomic_uint64_t g_deviceWatcherGeneration = 0;
std::atomic_bool g_deviceEnumerationCompleted = false;
std::atomic_bool g_devicePickerVisible = false;
std::atomic_bool g_initialAudioSinkActivationPending = true;
std::atomic_bool g_shuttingDown = false;

#include "Util.hpp"
#include "I18n.hpp"
#include "SettingsUtil.hpp"
#include "Direct2DSvg.hpp"
