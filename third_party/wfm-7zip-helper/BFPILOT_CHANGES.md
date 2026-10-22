# BFpilot helper fork

Source baseline is recorded in `BFPILOT_SOURCE.json`. Keep all original 7-Zip
and WFM notices and license texts. The helper is a separately linked program;
BFpilot's application license does not relicense it. Distributions must include
the corresponding helper source/build scripts and applicable notices.

The helper is built separately and embedded in BFpilot's single distributed ELF.
It starts on demand with a BFpilot-owned process, lock, Unix socket and framed
protocol identity (BF7Z/1 26.03-2-bf2). It never kills or replaces another daemon.
PS5 filesystem at-calls use BFpilot's checked syscall adapter. The build enables
C++ exceptions explicitly and uses the workspace SDK wrappers and response-file
linking. Source dependencies are tracked. TAR.ZST/TZST compound expansion is
included alongside the original compressed TAR variants.

The local service permits one extraction and at most eight retained tasks.
Completed tasks remain available for attach until the coordinator acknowledges
checked publication or cleanup. Idle shutdown is explicit; active tasks cannot
be stopped by that protocol. BFpilot persists the source/stage/task identity
before dispatch and verifies it on reconnect. No public network listener or
dependency on WFM's original daemon is introduced. Native R8 tests cover format
routing, failures, cancellation and coordinator restart during finalization.

The source-only reference and unchanged-engine controls remain available outside
the product tree for fair comparison. Do not label local modifications as an
unchanged upstream benchmark.
