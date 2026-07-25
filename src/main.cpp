#include <cstdio>

// Scaffold entry point. Proves the toolchain, flags, and sanitisers link and
// run. Real logging becomes a core facility in M0 commit 2, and stdio is then
// confined to that one place per CLAUDE.md section 3. This printf goes away
// with it.
int main() {
    std::printf("unionized_mech: M0 scaffold build and run OK\n");
    return 0;
}
