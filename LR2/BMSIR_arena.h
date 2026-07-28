#pragma once

struct game;
struct sqlite3;

namespace openlr2::arena {

void Initialize(game* gameState);
void Shutdown();
void Tick(game* gameState, sqlite3* songDatabase);
void DrawOverlay(const game* gameState);
bool ConsumePreparedChart(game* gameState, sqlite3* songDatabase);
void ApplyPlaySettings(game* gameState);
bool WaitForSynchronizedStart(game* gameState);
bool IsArenaPlayActive();
bool BlocksAbortInput();
bool BlocksQuickRestart();
bool IgnorePlayStartInputDelay();

} // namespace openlr2::arena
