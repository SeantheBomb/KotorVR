// KotOR's GUI manager state: which game screens are shown over the world. See gui.cpp.
#pragma once

struct GuiState {
    int modalCount;          // panels on the modal stack
    int menuPanels;          // shown panels that are game screens (not the HUD or a conversation / cutscene panel)
    unsigned long menuVtbl;  // vtable of one of them (identifies the screen), 0 if none
};

// Reads the GUI manager (logs changes). Game thread.
GuiState Gui_State();
