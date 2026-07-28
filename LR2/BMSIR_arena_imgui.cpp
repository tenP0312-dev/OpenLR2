#include "BMSIR_arena_imgui.h"

#ifdef _WIN32

#include "BMSIR_arena_log.h"

#include <DxLib.h>
#include <d3d11.h>
#include <d3d9.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include <filesystem>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
	HWND window,
	UINT message,
	WPARAM wParam,
	LPARAM lParam);

namespace openlr2::arena::imgui_overlay {
namespace {

enum class Renderer {
	None,
	Direct3D9,
	Direct3D11,
};

bool g_initialized{};
bool g_interactive{};
Renderer g_renderer{Renderer::None};

LRESULT CALLBACK ArenaWindowProc(
	HWND window,
	UINT message,
	WPARAM wParam,
	LPARAM lParam)
{
	if (!g_initialized) return 0;
	const LRESULT handled = ImGui_ImplWin32_WndProcHandler(
		window,
		message,
		wParam,
		lParam);
	if (handled != 0 && g_interactive) {
		SetUseHookWinProcReturnValue(TRUE);
		return handled;
	}
	return 0;
}

void ConfigureStyle()
{
	ImGui::StyleColorsDark();
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 3.0f;
	style.ChildRounding = 2.0f;
	style.FrameRounding = 2.0f;
	style.PopupRounding = 2.0f;
	style.ScrollbarRounding = 2.0f;
	style.GrabRounding = 2.0f;
	style.WindowBorderSize = 1.0f;
	style.FrameBorderSize = 1.0f;
	style.WindowPadding = ImVec2(10.0f, 9.0f);
	style.FramePadding = ImVec2(7.0f, 4.0f);
	style.ItemSpacing = ImVec2(7.0f, 5.0f);
	style.Colors[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.035f, 0.055f, 0.96f);
	style.Colors[ImGuiCol_ChildBg] = ImVec4(0.055f, 0.055f, 0.075f, 0.82f);
	style.Colors[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.08f, 0.12f, 1.0f);
	style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.08f, 0.18f, 0.28f, 1.0f);
	style.Colors[ImGuiCol_Header] = ImVec4(0.10f, 0.30f, 0.46f, 0.75f);
	style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.15f, 0.44f, 0.68f, 0.90f);
	style.Colors[ImGuiCol_Button] = ImVec4(0.10f, 0.30f, 0.46f, 0.82f);
	style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.15f, 0.44f, 0.68f, 1.0f);
	style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.12f, 0.36f, 0.56f, 1.0f);
	style.Colors[ImGuiCol_CheckMark] = ImVec4(0.35f, 0.78f, 1.0f, 1.0f);
}

void LoadJapaneseFont()
{
	ImGuiIO& io = ImGui::GetIO();
	const char* candidates[] = {
		"C:\\Windows\\Fonts\\meiryo.ttc",
		"C:\\Windows\\Fonts\\YuGothM.ttc",
		"C:\\Windows\\Fonts\\msgothic.ttc",
	};
	for (const char* path : candidates) {
		std::error_code error;
		if (!std::filesystem::exists(path, error)) continue;
		if (io.Fonts->AddFontFromFileTTF(
				path,
				16.0f,
				nullptr,
				io.Fonts->GetGlyphRangesJapanese())) {
			return;
		}
	}
	io.Fonts->AddFontDefault();
}

bool InitializeRenderer()
{
	const int version = GetUseDirect3DVersion();
	if (version == DX_DIRECT3D_9 || version == DX_DIRECT3D_9EX) {
		auto* device = const_cast<IDirect3DDevice9*>(
			static_cast<const IDirect3DDevice9*>(GetUseDirect3DDevice9()));
		if (!device || !ImGui_ImplDX9_Init(device)) return false;
		g_renderer = Renderer::Direct3D9;
		return true;
	}
	if (version == DX_DIRECT3D_11) {
		auto* device = const_cast<ID3D11Device*>(
			static_cast<const ID3D11Device*>(GetUseDirect3D11Device()));
		auto* context = const_cast<ID3D11DeviceContext*>(
			static_cast<const ID3D11DeviceContext*>(GetUseDirect3D11DeviceContext()));
		if (!device || !context || !ImGui_ImplDX11_Init(device, context)) return false;
		g_renderer = Renderer::Direct3D11;
		return true;
	}
	return false;
}

} // namespace

bool Initialize()
{
	if (g_initialized) return true;
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = "LR2files/Config/bmsir-arena-ui.ini";
	io.LogFilename = nullptr;
	ConfigureStyle();
	LoadJapaneseFont();
	if (!ImGui_ImplWin32_Init(GetMainWindowHandle()) || !InitializeRenderer()) {
		LogEvent("imgui_initialize_failed", {
			{"direct3d_version", GetUseDirect3DVersion()},
		});
		if (g_renderer == Renderer::Direct3D9) ImGui_ImplDX9_Shutdown();
		else if (g_renderer == Renderer::Direct3D11) ImGui_ImplDX11_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		g_renderer = Renderer::None;
		return false;
	}
	SetHookWinProc(ArenaWindowProc);
	g_initialized = true;
	LogEvent("imgui_initialized", {
		{"direct3d_version", GetUseDirect3DVersion()},
	});
	return true;
}

void Shutdown()
{
	if (!g_initialized) return;
	SetHookWinProc(nullptr);
	if (g_renderer == Renderer::Direct3D9) ImGui_ImplDX9_Shutdown();
	else if (g_renderer == Renderer::Direct3D11) ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	g_renderer = Renderer::None;
	g_initialized = false;
	g_interactive = false;
}

bool BeginFrame(const bool interactive)
{
	if (!g_initialized && !Initialize()) return false;
	g_interactive = interactive;
	ImGuiIO& io = ImGui::GetIO();
	io.MouseDrawCursor = interactive;
	if (g_renderer == Renderer::Direct3D9) ImGui_ImplDX9_NewFrame();
	else if (g_renderer == Renderer::Direct3D11) ImGui_ImplDX11_NewFrame();
	else return false;
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	return true;
}

void EndFrame()
{
	if (!g_initialized) return;
	ImGui::Render();
	if (g_renderer == Renderer::Direct3D9) {
		ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
	}
	else if (g_renderer == Renderer::Direct3D11) {
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	}
}

bool Available()
{
	return g_initialized;
}

bool WantsMouseCapture()
{
	return g_initialized && g_interactive && ImGui::GetIO().WantCaptureMouse;
}

} // namespace openlr2::arena::imgui_overlay

#else

namespace openlr2::arena::imgui_overlay {

bool Initialize() { return false; }
void Shutdown() {}
bool BeginFrame(bool) { return false; }
void EndFrame() {}
bool Available() { return false; }
bool WantsMouseCapture() { return false; }

} // namespace openlr2::arena::imgui_overlay

#endif
