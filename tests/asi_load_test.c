/* asi_load_test.c - the shipping mgs4vr.asi must be fully inert when loaded
 * into anything that is not mgs4.exe: no log directory, no file, no hooks.
 * Loads build\mgs4vr.asi into this test process, waits for its worker
 * thread to bail out, checks that no logs\mgs4vr.log appeared next to the
 * test exe, and unloads it. Exit code 0 = pass. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    char dir[MAX_PATH], log[MAX_PATH];
    const char *asi = argc > 1 ? argv[1] : "..\\build\\mgs4vr.asi";
    HMODULE m;
    char *p;
    GetModuleFileNameA(NULL, dir, sizeof(dir));
    p = strrchr(dir, '\\'); if (p) *p = 0;
    _snprintf_s(log, sizeof(log), _TRUNCATE, "%s\\logs\\mgs4vr.log", dir);
    DeleteFileA(log);
    m = LoadLibraryA(asi);
    if (!m) { printf("FAIL: could not load %s (error %lu)\n", asi, GetLastError()); return 1; }
    Sleep(500);
    if (GetFileAttributesA(log) != INVALID_FILE_ATTRIBUTES) {
        printf("FAIL: %s was created although the host is not mgs4.exe\n", log);
        return 1;
    }
    FreeLibrary(m);
    printf("PASS: asi_load_test (inert outside mgs4.exe)\n");
    return 0;
}
