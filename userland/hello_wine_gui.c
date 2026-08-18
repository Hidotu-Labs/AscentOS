#include <windows.h>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    MessageBoxA(NULL, "Hello from Wine on AvoryOS!", "Wine on AvoryOS", MB_OK | MB_ICONINFORMATION);
    return 0;
}
