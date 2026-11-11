# Intel Mac port

The existing Mac host is a native executor: the rewritten Switch modules are
AArch64 and call the host's HLE functions through the Apple AArch64 ABI. An
x86_64 build therefore needs a CPU boundary, not only `ARCHS = arm64 x86_64`.

The port keeps the Apple-silicon path unchanged and adds these layers on Intel:

1. Dynarmic's AArch64 frontend with its x86_64 JIT backend.
2. A register/stack bridge for guest calls, HLE imports and Horizon SVCs.
3. A translated path for `nn::jit` generated code.
4. Intel-safe NVN and thread entry points, followed by full game bring-up.

The CPU boundary is now vendored under `dm/third_party/dynarmic` and has a
standalone smoke test:

```sh
make -C dm/host/intel check
```

The current milestone executes AArch64 code on x86_64, validates the AArch64
register-to-SysV ABI bridge (including stack arguments and indirect results),
exposes the rewritten TLS slots and synthetic HLE/gate stubs, routes startup
and guest-thread entry points through the translator, and avoids ARM-only codec,
GL/Vulkan and NVN assembly in the Intel target. The build script can select
this backend on an Intel Mac, but gameplay is still experimental and has not
been validated here: the complete macOS host build, thread pause/context
snapshots, all composite/variadic ABIs, runtime-JIT generated code, and a real
Switch 1/2 game run remain to be tested.

Dynarmic is used under its 0BSD license, preserved in
`dm/third_party/dynarmic/LICENSE.txt`.
