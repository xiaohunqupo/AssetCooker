// Dear ImGui: standalone example application for DirectX 11

// Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/ folder).
// - Introduction, links and more at the top of imgui.cpp

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>

#include "UI.h"
#include "App.h"
#include "Debug.h"
#include "FileSystem.h"
#include "CookingSystem.h"
#include "RuleReader.h"
#include "Ticks.h"


// Data
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Helper struct to measure CPU/GPU times of the UI updates.
struct FrameTimer
{
	static constexpr int cGPUHistorySize                    = 32;
	static constexpr int cCPUHistorySize                    = 32;
	static constexpr int cFrameHistorySize                  = 256;

    ID3D11Query*         mDisjointQuery[cGPUHistorySize]    = {};
	ID3D11Query*         mStartQuery   [cGPUHistorySize]    = {};
	ID3D11Query*         mEndQuery     [cGPUHistorySize]    = {};
	double               mGPUTimesMS   [cGPUHistorySize]    = {}; // In Milliseconds.
	double               mCPUTimesMS   [cCPUHistorySize]    = {}; // In Milliseconds.
	double               mFrameTimesS  [cFrameHistorySize]  = {}; // In Seconds.
	uint64               mFrameIndex                        = 0;
	Timer                mCPUTimer;

	void Init()
	{
 		for (auto& query : mDisjointQuery)
		{
			D3D11_QUERY_DESC desc = { D3D11_QUERY_TIMESTAMP_DISJOINT };
			g_pd3dDevice->CreateQuery(&desc, &query);
		}

		for (auto& query : mStartQuery)
		{
			D3D11_QUERY_DESC desc = { D3D11_QUERY_TIMESTAMP };
			g_pd3dDevice->CreateQuery(&desc, &query);
		}

		for (auto& query : mEndQuery)
		{
			D3D11_QUERY_DESC desc = { D3D11_QUERY_TIMESTAMP };
			g_pd3dDevice->CreateQuery(&desc, &query);
		}

		mFrameIndex = 0;
	}

	void Shutdown()
	{
		for (auto& query : mDisjointQuery)
			query->Release();

		for (auto& query : mStartQuery)
			query->Release();

		for (auto& query : mEndQuery)
			query->Release();

		*this = {};
	}

	void Reset()
	{
		for (auto& time : mGPUTimesMS)
			time = 0;
		for (auto& time : mCPUTimesMS)
			time = 0;
		for (auto& time : mFrameTimesS)
			time = 0;
		mFrameIndex  = 0;
	}

	void StartFrame(double inWaitTime = 0)
	{
		// Frame time is between two StartFrame.
		// Skip the first frame because there is no previous StartFrame.
		if (mFrameIndex > 0)
			mFrameTimesS[mFrameIndex % cFrameHistorySize] = gTicksToSeconds(mCPUTimer.GetTicks()) - inWaitTime;

		// Reset the timer for this frame.
		mCPUTimer.Reset();

		// Start the queries for GPU time.
		auto gpu_frame_index = mFrameIndex % cGPUHistorySize;
		g_pd3dDeviceContext->Begin(mDisjointQuery[gpu_frame_index]);
		g_pd3dDeviceContext->End(mStartQuery[gpu_frame_index]);
	}

	void EndFrame()
	{
		// CPU time is between Strt and EndFrame.
		mCPUTimesMS[mFrameIndex % cCPUHistorySize] = gTicksToMilliseconds(mCPUTimer.GetTicks());

		// End the GPU queries.
		auto gpu_frame_index = mFrameIndex % cGPUHistorySize;
		g_pd3dDeviceContext->End(mEndQuery[gpu_frame_index]);
		g_pd3dDeviceContext->End(mDisjointQuery[gpu_frame_index]);

		// Update the frame index.
		mFrameIndex++;

		// Get the GPU query data for the oldest frame.
		// This is a bit sloppy, we don't check the return value, but it's unlikely to fail with >5 frames of history, and if it fails the GPU time will just be zero.
		gpu_frame_index = mFrameIndex % cGPUHistorySize;
        UINT64 start_time = 0;
		g_pd3dDeviceContext->GetData(mStartQuery[gpu_frame_index], &start_time, sizeof(start_time), 0);

        UINT64 end_time = 0;
		g_pd3dDeviceContext->GetData(mEndQuery[gpu_frame_index], &end_time, sizeof(end_time), 0);

        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint_data = { .Frequency = 1, .Disjoint = TRUE };
		g_pd3dDeviceContext->GetData(mDisjointQuery[gpu_frame_index], &disjoint_data, sizeof(disjoint_data), 0);

        if (disjoint_data.Disjoint == FALSE)
        {
            UINT64 delta = end_time - start_time;
            mGPUTimesMS[gpu_frame_index] = (double)delta * 1000.0 / (double)disjoint_data.Frequency;
        }
		else
		{
			mGPUTimesMS[gpu_frame_index] = 0.0; // Better show 0 than an unreliable number.
		}
	}

	double GetGPUAverageMilliseconds() const
	{
		int    count = 0;
		double sum   = 0;
		for (double time : mGPUTimesMS)
		{
			if (time == 0.0) continue; // Skip invalid times.

			count++;
			sum += time;
		}
		return sum / count;
	}

	double GetCPUAverageMilliseconds() const
	{
		int    count = 0;
		double sum   = 0;
		for (double time : mCPUTimesMS)
		{
			if (time == 0.0) continue; // Skip invalid times.

			count++;
			sum += time;
		}
		return sum / count;
	}

	int GetAverageFPS() const
	{
		int    count = 0;
		double sum   = 0;
		for (double time : mFrameTimesS)
		{
			if (time == 0.0) continue; // Skip invalid times.

			count++;
			sum += time;
		}
		double av_frame_time = sum / count;
		return (int)ceil(1.0 / av_frame_time); // Ceil because it's annoying to see it flicker to 59.
	}
};


// Main code
int WinMain(
  HINSTANCE hInstance,
  HINSTANCE hPrevInstance,
  LPSTR     lpCmdLine,
  int       nShowCmd
)
{
	// TODO link static runtime to avoid having to install vcredist

	ImGui_ImplWin32_EnableDpiAwareness();

	gApp.Init();

	// TODO move that inside App?
	// TODO load window size/pos from config file?
	// Create application window
	WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ImGui Example", nullptr };
	::RegisterClassExW(&wc);
	wchar_t name_buffer[128];
	auto name_wchar = gUtf8ToWideChar(cAppName, name_buffer);
	HWND hwnd = ::CreateWindowW(
		wc.lpszClassName, 
		name_wchar.value_or(L"").data(), 
		WS_OVERLAPPEDWINDOW, 
		CW_USEDEFAULT, CW_USEDEFAULT, // pos
		CW_USEDEFAULT, CW_USEDEFAULT, // size
		nullptr, nullptr, wc.hInstance, nullptr);

	gApp.mMainWindowHwnd = hwnd;

	// Initialize Direct3D
	// TODO start the threads before doing this because that's slow (~300ms)
	if (!CreateDeviceD3D(hwnd))
		gApp.FatalError("Failed to create D3D device - {}", GetLastErrorString());

	// Show the window
	::ShowWindow(hwnd, SW_SHOWDEFAULT);
	::UpdateWindow(hwnd);

	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking
	//io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;       // Enable Multi-Viewport / Platform Windows
	//io.ConfigViewportsNoAutoMerge = true;
	//io.ConfigViewportsNoTaskBarIcon = true;
	//io.ConfigViewportsNoDefaultParent = true;
	//io.ConfigDockingAlwaysTabBar = true;
	//io.ConfigDockingTransparentPayload = true;
	//io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleFonts;     // FIXME-DPI: Experimental. THIS CURRENTLY DOESN'T WORK AS EXPECTED. DON'T USE IN USER APP!
	//io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleViewports; // FIXME-DPI: Experimental.

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();
	//ImGui::StyleColorsLight();

#ifdef IMGUI_HAS_VIEWPORT
	// When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
	ImGuiStyle& style = ImGui::GetStyle();
	if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
	{
		style.WindowRounding = 0.0f;
		style.Colors[ImGuiCol_WindowBg].w = 1.0f;
	}
#endif

	// Set the DPI scale once, then it'll be updated by the WM_DPICHANGED message.
	gUISetDPIScale(ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd));

	// Setup Platform/Renderer backends
	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

	FrameTimer fame_timer;
	fame_timer.Init();

	constexpr int cIdleFramesDelay = 2; // TODO should ideally be zero, but needs some fixes, check the other todos
	int           idle_frames      = 0;

	// Main loop
	while (!gApp.IsExitReady())
	{
		if (!gCookingSystem.IsIdle())
			idle_frames = 0;
		else
			idle_frames++;

		// We've been idle enough time, don't draw the UI and wait for a window event instead.
		if (idle_frames > cIdleFramesDelay)
		{
			Timer idle_timer;
			while (true)
			{
				// Wait one second for a window event.
				constexpr int timeout = 1000; // One second.
				if (MsgWaitForMultipleObjects(0, nullptr, FALSE, timeout, QS_ALLINPUT) == WAIT_OBJECT_0)
					break; // There's an event in the queue, go call PeekMessage!

				// After one second, check if the cooking system is still idle (may have been files modified).
				if (!gCookingSystem.IsIdle())
					break;
			}

			idle_frames = 0;

			// Start a new frame but include how much time we've spend waiting to not skew the FPS.
			double wait_time = gTicksToSeconds(idle_timer.GetTicks());
			fame_timer.StartFrame(wait_time);
		}
		else
		{
			fame_timer.StartFrame();
		}

		// If we're going idle next frame, draw one last frame that says we're going idle (by setting the FPS to 0).
		// That only works if there's at least 1 frame delay.
		if (cIdleFramesDelay > 0 && idle_frames == cIdleFramesDelay)
			gUILastFrameStats = {};

		// Poll and handle messages (inputs, window resize, etc.)
		// See the WndProc() function below for our to dispatch events to the Win32 backend.
		MSG msg;
		while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
		{
			// If there's any message to process, we're not idle. The mouse might be moving over the UI, etc.
			idle_frames = 0;

			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
		}
		if (gApp.IsExitReady())
			break;

		// If the window is minimized, never draw.
		if (IsIconic(hwnd))
		{
			// We're not going to call EndFrame, so reset the timer.
			fame_timer.Reset();
			continue;
		}

		// Handle window resize (we don't resize directly in the WM_SIZE handler)
		if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
		{
			CleanupRenderTarget();
			g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
			g_ResizeWidth = g_ResizeHeight = 0;
			CreateRenderTarget();
		}

		gUIUpdate();

		// Start the Dear ImGui frame
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		gDrawMainMenuBar();
		gDrawMain();

		// Rendering
		ImGui::Render();
		g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

#ifdef IMGUI_HAS_VIEWPORT
		// Update and Render additional Platform Windows
		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			ImGui::UpdatePlatformWindows();
			ImGui::RenderPlatformWindowsDefault();
		}
#endif
		fame_timer.EndFrame();

		gUILastFrameStats.mCPUMilliseconds = fame_timer.GetCPUAverageMilliseconds();
		gUILastFrameStats.mGPUMilliseconds = fame_timer.GetGPUAverageMilliseconds();
		gUILastFrameStats.mFPS             = fame_timer.GetAverageFPS();

		g_pSwapChain->Present(1, 0); // Present with vsync
	}

	gFileSystem.StopMonitoring();

	// Cleanup
	fame_timer.Shutdown();
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	CleanupDeviceD3D();
	::DestroyWindow(hwnd);
	::UnregisterClassW(wc.lpszClassName, wc.hInstance);

	return 0;
}

// Helper functions
bool CreateDeviceD3D(HWND hWnd)
{
	// Setup swap chain
	DXGI_SWAP_CHAIN_DESC sd;
	ZeroMemory(&sd, sizeof(sd));
	sd.BufferCount = 2;
	sd.BufferDesc.Width = 0;
	sd.BufferDesc.Height = 0;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hWnd;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	UINT createDeviceFlags = 0;
	//createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
	D3D_FEATURE_LEVEL featureLevel;
	const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
	HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
	if (res == DXGI_ERROR_UNSUPPORTED) // Try high-performance WARP software driver if hardware is not available.
		res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
	if (res != S_OK)
		return false;

	CreateRenderTarget();
	return true;
}

void CleanupDeviceD3D()
{
	CleanupRenderTarget();
	if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
	if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
	if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
	ID3D11Texture2D* pBackBuffer;
	g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
	g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
	pBackBuffer->Release();
}

void CleanupRenderTarget()
{
	if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0 // From Windows SDK 8.1+ headers
#endif

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 message handler
// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return true;

	switch (msg)
	{
	case WM_SIZE:
		if (wParam == SIZE_MINIMIZED)
			return 0;
		g_ResizeWidth = (UINT)LOWORD(lParam); // Queue resize
		g_ResizeHeight = (UINT)HIWORD(lParam);
		return 0;
	case WM_SYSCOMMAND:
		if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
			return 0;
		break;
		
	case WM_DESTROY:
		::PostQuitMessage(0);
		[[fallthrough]];
	case WM_QUIT:
		gApp.RequestExit();
		return 0;
	case WM_DPICHANGED:

		// Update the UI DPI scale.
		const int   dpi       = HIWORD(wParam);
		const float dpi_scale = (float)dpi / 96.0f;
		gUISetDPIScale(dpi_scale);

#ifdef IMGUI_HAS_VIEWPORT
		if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DpiEnableScaleViewports)
		{
			//const int dpi = HIWORD(wParam);
			//printf("WM_DPICHANGED to %d (%.0f%%)\n", dpi, (float)dpi / 96.0f * 100.0f);
			const RECT* suggested_rect = (RECT*)lParam;
			::SetWindowPos(hWnd, nullptr, suggested_rect->left, suggested_rect->top, suggested_rect->right - suggested_rect->left, suggested_rect->bottom - suggested_rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
		}
#endif
		break;
	}
	return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
