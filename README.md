# Isiliel

Isiliel is a small, embeddable scripting language implemented in C++17. It
uses a classic pipeline:

```
source -> lexer -> bytecode compiler -> stack-based virtual machine
```

The same executable provides a script runner, an interactive REPL, a
bytecode disassembler, and a C++ embedding API.

- **Authors:** Eben (AI-assisted)
- **Version:** 1.0
- **Date:** 2026-11-08
- **Language:** C++17
- **License:** MIT (see [LICENSE](LICENSE))

## Features

- Exact 64-bit integers, double-precision floating point, and complex
  numbers with the `j` suffix.
- Immutable strings with escapes, indexing, slicing, UTF-8 helpers, and a
  Lua-style pattern subset.
- Arduino String-style free functions and a C99 `math.h` subset.
- 0-based lists/matrices with slicing, `append`, `sort`, and basic linear
  algebra (`det`, `inv`, `eigvals`, `eigvecs`, ...).
- Tables/dictionaries with text, numeric, or boolean keys.
- First-class functions, multiple returns, varargs, and anonymous
  closures.
- Structs and classes with single inheritance, `init` constructors, and
  `super`.
- Metatables and metamethods (`__index`, `__call`, `__add`, `__eq`,
  `__tostring`, `__gc`).
- Cycle collection by trial deletion.
- Modules through `include`, `require`, `load`, `dofile`, and native
  `.mod` shared libraries.
- System threads and cooperative coroutines.
- Processing-style 2D graphics with a headless software backend or an
  optional raylib window backend.
- REPL, automated tests, ASan/UBSan builds, deterministic fuzzing, and
  Lua 5.4 benchmarks.
- C++ embedding API with sandboxing, output capture, step limits, and
  host function registration.

## Requirements

### Required

- A C++17 compiler (`g++` or `clang++`).
- GNU Make.
- `ar` (usually provided by the toolchain / `binutils`).
- POSIX threads and `dlopen`/`dlsym` on Linux (`-lpthread -ldl` are used
  by the build).

### Optional

- **raylib** for real windows, GPU-backed graphics, and audio output.
  The Makefile detects it through `pkg-config`. On Debian/Ubuntu:

  ```bash
  sudo apt install libraylib-dev
  ```

  A local raylib installation under `third_party/raylib-install` is also
  detected automatically.
- **Lua 5.4** only for `make bench`. The repository expects a local copy
  under `third_party/lua5.4`.
- **LaTeX** (`pdflatex`) only to rebuild the manual in `docs/`.

If raylib is not available, everything still builds and runs with the
headless `software` backend (images are written as `.ppm`).

## Build

From the project root:

```bash
make                 # debug build: build/isirun (-O0 -g3)
make release         # optimized build: -O2 -DNDEBUG -flto
make embedded        # small headless build: -Os, no raylib
```

Raylib selection:

```bash
make USE_RAYLIB=auto   # default: use raylib if pkg-config finds it
make USE_RAYLIB=1      # require raylib (fail if not found)
make USE_RAYLIB=0      # disable raylib
```

Other targets:

```bash
make test            # automated test suite
make asan            # test suite with AddressSanitizer + UBSan
make fuzz            # deterministic fuzzer
make bench           # benchmark against Lua 5.4
make clean           # remove build artifacts
```

> The `make test`, `make asan`, `make fuzz`, and `make bench` targets
> expect the `tests/`, `bench/`, and `third_party/lua5.4` directories.
> Those directories are not included in every checkout; the core
> `make`, `make release`, and `make embedded` builds do not need them.

The main executable is produced at `build/isirun`. The static library is
`build/libisiliel.a`.

## Run

Run a script:

```bash
./build/isirun scripts/script_10_hello_complexo.isi
```

Disassemble a script without executing it:

```bash
./build/isirun --disasm scripts/script_1_matriz_lu.isi
```

Start the interactive REPL:

```bash
./build/isirun
```

Example:

```text
// hello.isi
name = "world";
println("Hello, " + name + "!");
```

```bash
./build/isirun hello.isi
```

The language uses `;`-terminated statements, `//` line comments, and
`fn`/named function definitions. See the manual for the full language
reference.

## Documentation

- `docs/manual_isiliel.tex` — English manual/tutorial source (LaTeX).
- `docs/manual_isiliel.pdf` — compiled manual.
- `scripts/` — runnable examples covering the language, standard
  library, graphics, sound, threads, coroutines, and modules.

## Embedding

The public API is declared in `interp.h`:

```cpp
#include "interp.h"

int main() {
    InterpOptions opts;
    opts.capture = true;
    opts.step_limit = 100000;
    opts.allow_fileio = false;
    opts.allow_modules = false;

    InterpResult r = interp_run("println(6 * 7);", opts);
    return r.ok ? 0 : 1;
}
```

Link against `build/libisiliel.a` and the platform libraries used by the
build (`-lpthread -ldl`, plus raylib when enabled).

## AI Disclosure

This codebase and its documentation were developed with the use of
artificial intelligence (AI) tools. The authors reviewed and tested the
resulting code, but no guarantee is made regarding correctness,
completeness, security, or fitness for any particular purpose.

## License

This project is distributed under the MIT License. See [LICENSE](LICENSE)
for the full text.

Copyright (c) 2026 Eben

## Disclaimer

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

The authors provide the code as-is and accept no responsibility for any
use or misuse of it, including any direct or indirect damages resulting
from its use.
