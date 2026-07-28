#pragma once

namespace openlr2::arena::imgui_overlay {

bool Initialize();
void Shutdown();
bool BeginFrame(bool interactive);
void EndFrame();
bool Available();
bool WantsMouseCapture();
bool WantsKeyboardCapture();

} // namespace openlr2::arena::imgui_overlay
