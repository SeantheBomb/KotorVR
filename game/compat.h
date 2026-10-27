// Compatibility with programs that inject into games: keeps Discord's in-game overlay (DiscordHook.dll) out of
// KotOR unless kvr.ini [Compat] DiscordOverlay=1 (see compat.cpp).
#pragma once

void Compat_Install(bool allowDiscordOverlay);
