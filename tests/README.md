# luna-downloadmgr tests and hardening

Three layers, each answering a different question.

| Layer | Question | Runs where |
|---|---|---|
| `tests/unit/` | does the logic still do what it is supposed to? | host or target, via `ctest` |
| `tests/fuzz/` | do the parsers survive input nobody wrote a test for? | host, clang + sanitizers |
| `tests/stress/` | does the daemon survive its own state machine under load? | on the device |
| `tools/analyze.sh` | does it still compile clean on every target? | build host |

None of it is enabled by default, so an ordinary component build is unchanged.

## Hardening

`cmake/Hardening.cmake` adds exploit mitigations and extra warnings. Every flag
is probed with `check_cxx_compiler_flag` first, so it is safe on armv7, aarch64
and x86-64, and on both gcc and clang.

```
-DENABLE_HARDENING=ON     # default: stack protector + clash protection, CF
                          # protection on x86, _FORTIFY_SOURCE=3 (2 if the
                          # toolchain is older), PIE, full RELRO, noexecstack,
                          # -Werror=format-security
-DENABLE_WERROR=ON        # default OFF
-DSANITIZER=address,undefined
```

`-Werror=format-security` is there deliberately: a `%lu` applied to a `uint64_t`
is invisible on aarch64 and x86-64 and silently corrupts the rest of the
argument list on armv7, which is exactly the bug class this component shipped.

`_FORTIFY_SOURCE` earns its keep on its own. Reintroducing the unbounded
`strncpy` in `MemStringToBytes()` and running the unit tests gives:

```
[ RUN      ] MemStringToBytes.LongDigitRunsDoNotOverflowTheBuffer
*** buffer overflow detected ***: terminated
```

Note that `_FORTIFY_SOURCE` needs optimisation to do anything, so a `Debug`
build skips it, and ASan and the fortified wrappers disagree about redzones, so
`-DSANITIZER=address` turns it off.

To confirm the mitigations survived into the binary:

```sh
readelf -lWd LunaDownloadMgr | grep -E 'GNU_RELRO|GNU_STACK|BIND_NOW'
readelf -hW  LunaDownloadMgr | grep Type          # expect DYN (PIE)
```

## Unit tests

```sh
cmake -B build -DENABLE_TESTS=ON      # needs gtest in the sysroot
cmake --build build
ctest --test-dir build --output-on-failure
```

62 assertions over the parts that can be exercised without a live luna bus:

- `test_urlrep` — scheme extraction (what the download security check keys off),
  malformed URLs, query dissection, the directory-URL case that makes
  `download()` fall back to `mkstemp`.
- `test_pathutils` — `splitStringOnKey` / `splitFileAndPath` /
  `splitFileAndExtension` / `trimWhitespace`, including the empty and
  separator-only inputs that these index with `at()`, plus a sparse-file check
  that `filesizeOnFilesystem()` reports sizes above 2GB.
- `test_settings` — `validateDownloadPath()` (the only thing keeping the
  download directory inside `/var` or `/media`) and `MemStringToBytes()`.
- `test_taskjson` — that `DownloadTask::toJSON()` still emits every key
  `resumeDownload()` reads back, that the 64-bit counters survive the round
  trip, and that a URL full of quotes still serializes to parseable JSON.
  This is the test that would have caught `e_initialOffsetBytes` being written
  and `e_initialOffset` being read.
- `test_fsstatus` — the percentage arithmetic in `filesystemStatusCheck()`,
  including the zero-sized filesystem that `filesysStatusCheck {"path":"/proc"}`
  produces, and the connection-name round trip that the history db depends on.
- `test_jutil` — schema validation, against schema files the test writes itself
  so it does not need the component installed. Covers the accept and reject
  paths and, specifically, that `Error::code()` still distinguishes
  `Error::Parse` (not JSON) from `Error::Schema` (JSON the schema refuses),
  which is what a caller sees as `errorText`.
- `test_upload` — the multipart body, checked on the wire. Each case stands up a
  throwaway loopback HTTP server, runs a real transfer through the same easy
  handle `UploadTask` configured, and asserts on the request that arrived: the
  boundary, each part's `name=`, the inline data, the file contents and derived
  `filename=`, the per-part `Content-Type`, and that a part with no content type
  gets no empty `Content-Type` header. Also that a buffer upload's body survives
  its source `std::string` going out of scope.

`tests/unit/test_support.cpp` supplies `gMainLoop`, which the daemon defines in
`Main.cpp`. The tests link `LunaDownloadMgrCore`, which is every source *except*
`Main.cpp`, so there is exactly one `main()` in the binary.

In a cross build the test binaries are built for the target, so `ctest` on the
build host needs an emulator. `add_test` names the targets rather than paths, so
setting `CMAKE_CROSSCOMPILING_EMULATOR` is enough:

```sh
cmake -B build -DENABLE_TESTS=ON       -DCMAKE_CROSSCOMPILING_EMULATOR="qemu-aarch64;-L;/path/to/sysroot"
```

Without it, run them on the target, or configure a native build for the tests.

## Fuzzing

```sh
cmake -B build-fuzz -DENABLE_FUZZERS=ON \
      -DSANITIZER=address,undefined \
      -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz

# short deterministic pass over the checked-in corpus (also wired into ctest)
ctest --test-dir build-fuzz --output-on-failure

# an actual campaign. Note the argument order: libFuzzer writes newly
# discovered units into the FIRST directory and treats the rest as read-only,
# so keep the checked-in seeds second or they will grow by hundreds of files.
mkdir -p /tmp/ldm-corpus
./build-fuzz/tests/fuzz/fuzz_urlrep -jobs=8 -max_total_time=3600 \
      /tmp/ldm-corpus tests/fuzz/corpus/fuzz_urlrep
```

Four targets, one per untrusted-input surface:

- `fuzz_urlrep` — `UrlRep::fromUrl()`, the first code a caller-supplied download
  target reaches. Asserts the invariant `download()` relies on: a valid parse
  with a non-empty `resource` is used verbatim as a filename, so it must never
  contain `/`.
- `fuzz_header` — the response-header parsing from `cbHeader()`, fed as
  (pointer, length) with no terminator, because that is how libcurl hands it
  over. Asserts that `setUpdateInterval()` never produces 0 and that
  `setMimeType()` leaves no trailing CR/LF.
- `fuzz_memstring` — `MemStringToBytes()` against arbitrary config values.
- `fuzz_historyjson` — the field extraction `resumeDownload()` and
  `cancelFromHistory()` perform on a stored history row. Asserts that the
  resume offset arithmetic cannot exceed the received byte count (the unsigned
  subtraction that used to wrap) and that anything we serialize, we can parse.

Corpora under `tests/fuzz/corpus/<target>/` are seeds, not a coverage record;
they exist so a `ctest` run starts somewhere useful.

Baseline: 20,000 executions per target under ASan+UBSan, no findings.

## Stress

`tests/stress/` replaces the old `src/test/scripts/lots-of-downloads.sh`, which
issued 50,000 identical requests at a public FTP mirror — that exercises one
code path and depends on the internet.

`ldm-origin.py` is a local HTTP origin that produces the awkward cases on
demand. Run it anywhere the device can reach:

```sh
python3 tests/stress/ldm-origin.py --port 8099 --bind 0.0.0.0
```

| Route | Behaviour |
|---|---|
| `/ok/<bytes>` | 200, exact `Content-Length`, honours `Range` |
| `/slow/<bytes>/<kbps>` | throttled, so pause/resume/cancel have a window |
| `/nolength/<bytes>` | no `Content-Length`, so `bytesTotal` stays 0 |
| `/truncate/<bytes>` | promises `<bytes>`, sends half |
| `/drop/<bytes>` | connection closed mid-body |
| `/redirect/<n>` | `n` chained 302s (>5 must be refused) |
| `/redirect-loop` | 302 to itself |
| `/status/<code>` | that status code |
| `/huge` | `Content-Length` above 2^32 |
| `/weird-headers` | 8KB header values, duplicates, colons, quotes, UTF-8 |
| `/quote-name` | redirect whose target is full of JSON metacharacters |
| `/stall/<bytes>` | headers then silence, to trip `CURLOPT_LOW_SPEED_TIME` |
| `/flaky/<bytes>` | half the requests drop, so resume makes uneven progress |

Then, on the device:

```sh
./ldm-stress.sh --origin http://192.168.1.10:8099 --scenario all
./ldm-stress.sh --origin http://192.168.1.10:8099 --scenario leak --iterations 2000
```

Scenarios, and the failure each one is looking for:

| Scenario | Looking for |
|---|---|
| `smoke` | one request per origin behaviour still completes |
| `queue` | overfilling the queue, then draining it |
| `pause` | pausing an active transfer *with work queued behind it* — the shape that read the freed `DownloadTask` |
| `resume` | resuming after the partial file has been deleted — the unsigned wrap in `completedSize - initialOffset` |
| `cancel` | repeated start/cancel, watching RSS for the task leaked on the notification-failure path |
| `redirect` | chains at, below and past `MAXREDIRECTIONS`, plus a loop |
| `headers` | oversized/duplicated headers, `Content-Length` past 2^32, a stalled transfer |
| `badinput` | malformed payloads, path traversal in `targetDir`/`targetFilename`, unknown tickets, `filesysStatusCheck` on `/proc`, upload outside `/media/internal` — every reply must parse as JSON and the service must still be there |
| `leak` | many completed downloads; asserts descriptors return to their starting count and RSS does not grow by more than 50% |
| `fdstorm` | driving past 1023 open descriptors, which is where glibcurl's `lastPollFd[]` ends. Not part of `all`; run it explicitly, and raise `MaxConcurrent`/`MaxQueueLength` in `/etc/palm/downloadManager.conf` first or it will not get there |

Each scenario prints `PASS`/`FAIL` lines and the script exits non-zero if any
assertion failed or the service died.

For a stress run to be worth anything the daemon has to be built to notice
damage, so build it hardened, and with sanitizers if the target has the
runtimes:

```sh
cmake -B build -DENABLE_HARDENING=ON -DSANITIZER=address,undefined
```

## Static analysis and the multi-arch warning gate

```sh
tools/analyze.sh --sysroot /path/to/recipe-sysroot \
                 --cross   /path/to/bin/arm-webos-linux-gnueabi- \
                 --arch    armv7
```

Runs cppcheck (`--enable=all --check-level=exhaustive`), clang-tidy
(`bugprone-*`, `cert-*`, `clang-analyzer-*`, `concurrency-*`, ...),
`gcc -fanalyzer`, `clang --analyze`, and a `-Wall -Wextra -Wshadow -Wformat=2`
compile, and reports third-party deprecation notices separately so they cannot
mask a regression.

Run it for **all three** architectures. `uint64_t` is `unsigned long long` on
armv7 and `unsigned long` on aarch64 and x86-64, so format-string mismatches
only appear on the 32-bit target — which is how 24 of them accumulated
unnoticed.

With no `--sysroot` it runs cppcheck only, which still catches a useful amount
with no cross-compile setup.

## Warnings

The tree builds with **no warnings at all** - not even third-party deprecation
notices - under `-Wall -Wextra -Wshadow -Wformat=2` for armv7, aarch64 and
x86-64. Both deprecated dependency APIs have been migrated:

- libcurl's form API (`curl_formadd`, `CURLFORM_*`, `CURLOPT_HTTPPOST`,
  `curl_formfree`), deprecated since 7.56, is now `curl_mime_*` /
  `CURLOPT_MIMEPOST`. `test_upload` pins the resulting body on the wire.
- pbnjson's `JValue::begin()`/`end()`, `JDomParser(JResolver*)`,
  `JDomParser::parse(..., JErrorHandler*)` and `JSchemaFile` are now
  `JValue::children()`, `JSchema::fromFile()`, `JSchema::resolve()` at load
  time and `parse()` without an error handler. `test_jutil` pins the
  validation behaviour, including the error classification that the removed
  `JErrorHandler` used to provide.

Keeping it that way is what `-DENABLE_WERROR=ON` and `tools/analyze.sh` are
for. Run the analyzer for all three architectures before merging.
