#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>

#pragma comment(lib, "dbghelp.lib")

extern "C" void print_callstack(void)
{
    void* stack[64];
    HANDLE process = GetCurrentProcess();
    SYMBOL_INFO* symbol;
    DWORD frames;
    unsigned short i;

    SymInitialize(process, NULL, TRUE);
    frames = CaptureStackBackTrace(0, 64, stack, NULL);

    symbol = (SYMBOL_INFO*)calloc(sizeof(SYMBOL_INFO) + 256 * sizeof(char), 1);
    symbol->MaxNameLen = 255;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

    fprintf(stderr, "\n--- Call Stack ---\n");
    for (i = 0; i < frames; i++) {
        if (SymFromAddr(process, (DWORD64)(stack[i]), 0, symbol)) {
            fprintf(stderr, "%u: %s - 0x%0llX\n",
                    frames - i - 1, symbol->Name, symbol->Address);
        } else {
            fprintf(stderr, "%u: [unknown symbol] - 0x%0llX\n",
                    frames - i - 1, (unsigned long long)stack[i]);
        }
    }
    fprintf(stderr, "--- End Stack ---\n\n");

    free(symbol);
}
#endif
