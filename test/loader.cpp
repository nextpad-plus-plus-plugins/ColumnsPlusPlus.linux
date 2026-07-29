// dlopen smoke test: load the built plugin, confirm getName() and the menu
// (getFuncsArray) without needing the Nextpad++ host.
//
// Usage: loader <path to ColumnsPlusPlus.so>

#include "NppPluginInterfaceLinux.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <so>\n", argv[0]); return 2; }
    // RTLD_LAZY | RTLD_LOCAL — exactly what the host's load_plugin() uses.
    void *h = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
    if (!h) { printf("dlopen FAIL: %s\n", dlerror()); return 1; }

    auto pGetName  = (const char *(*)())dlsym(h, "getName");
    auto pGetFuncs = (FuncItem * (*)(int *)) dlsym(h, "getFuncsArray");
    auto pSetInfo  = (void (*)(LinuxHostNppData))dlsym(h, "setInfo");
    if (!pGetName || !pGetFuncs || !pSetInfo) { printf("dlsym FAIL\n"); return 1; }

    LinuxHostNppData nd{};
    nd.hostMsg = [](unsigned int, unsigned long, long) -> long { return 0; };
    pSetInfo(nd);

    printf("getName: '%s'\n", pGetName());
    int n = 0;
    FuncItem *f = pGetFuncs(&n);
    printf("nbFunc: %d\n", n);
    int seps = 0;
    for (int i = 0; i < n; ++i) {
        printf("  [%2d] '%s'%s\n", i, f[i]._itemName, f[i]._pFunc ? "" : "   <separator>");
        if (!strcmp(f[i]._itemName, "-") && !f[i]._pFunc) ++seps;
    }
    printf("sizeof(FuncItem): %zu (host expects 80)\n", sizeof(FuncItem));

    bool ok = (n == 38) && (sizeof(FuncItem) == 80) && (seps == 7) &&
              !strcmp(pGetName(), "Columns++") &&
              !strcmp(f[0]._itemName, "Elastic tabstops") &&
              !strcmp(f[3]._itemName, "Search...") &&
              !strcmp(f[37]._itemName, "Help/About...");
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
