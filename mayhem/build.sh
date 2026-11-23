#!/usr/bin/env bash
#
# mayhem/build.sh — build RVVM's fuzz harnesses and the known-answer selftest.
#
# Runs in the commit image as `mayhem` in /mayhem. Outputs:
#   /mayhem/mayhem/out/fuzz/        sanitized librvvm_static.a (ASan+UBSan, SanCov, DWARF 3)
#   /mayhem/mayhem/out/release/     librvvm_static.a with upstream's normal flags (for the selftest)
#   /mayhem/mayhem/out/gen/         guest stubs assembled from mayhem/harnesses/guest/*.S
#   /mayhem/fuzz_<target>[-standalone], /mayhem/mayhem/out/rvvm_selftest
# Everything under mayhem/out and /mayhem/fuzz_* is gitignored (upstream ignores
# every top-level entry it does not whitelist), so `git clean -ffdX` resets it.
set -euo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
: "${SRC:=/mayhem}"
: "${STANDALONE_FUZZ_MAIN:=/opt/mayhem/StandaloneFuzzTargetMain.c}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"
OUT="$SRC/mayhem/out"
GEN="$OUT/gen"
mkdir -p "$GEN"

# RVVM build options shared by both builds: no GUI (needs X11/Wayland), no seccomp
# isolation (it would also confine the fuzzer/test process), no LTO (bitcode
# archives do not link with the default linker), JIT/net/gdbstub on.
RVVM_OPTS=(USE_LTO=0 USE_GUI=0 USE_SOUND=0 USE_ISOLATION=0 USE_LIB=0 USE_LIB_STATIC=1)

# 1) Guest-side stubs and the selftest guest program (RISC-V, by name through PATH).
asm_bin() { # <src.S> <xlen> <out.bin>
    local march=rv64gc abi=lp64
    if [ "$2" = 32 ]; then march=rv32gc; abi=ilp32; fi
    clang --target=riscv$2-unknown-elf -march=$march -mabi=$abi -nostdlib -c "$1" -o "$3.o"
    ld.lld -m elf${2}lriscv -Ttext=0x80000000 -e _start "$3.o" -o "$3.elf"
    llvm-objcopy -O binary "$3.elf" "$3"
}
to_c_array() { # <name> <file>
    printf 'static const unsigned char %s[] = {\n' "$1"
    od -An -v -tx1 "$2" | sed -e 's/ \([0-9a-f][0-9a-f]\)/0x\1,/g'
    printf '};\n'
}
H="$SRC/mayhem/harnesses/guest"
asm_bin "$H/stub_code.S" 64 "$GEN/stub_code64.bin"
asm_bin "$H/stub_code.S" 32 "$GEN/stub_code32.bin"
asm_bin "$H/stub_mmio.S" 64 "$GEN/stub_mmio64.bin"
asm_bin "$H/stub_off.S"  64 "$GEN/stub_off64.bin"
{
    to_c_array stub_code64 "$GEN/stub_code64.bin"
    to_c_array stub_code32 "$GEN/stub_code32.bin"
    to_c_array stub_mmio64 "$GEN/stub_mmio64.bin"
    to_c_array stub_off64  "$GEN/stub_off64.bin"
} > "$GEN/guest_stubs.h.tmp"
mv "$GEN/guest_stubs.h.tmp" "$GEN/guest_stubs.h"
asm_bin "$SRC/mayhem/selftest/test_guest.S" 64 "$GEN/guest64.bin"
asm_bin "$SRC/mayhem/selftest/test_guest.S" 32 "$GEN/guest32.bin"
{
    to_c_array guest64 "$GEN/guest64.bin"
    to_c_array guest32 "$GEN/guest32.bin"
} > "$GEN/selftest_guest.h.tmp"
mv "$GEN/selftest_guest.h.tmp" "$GEN/selftest_guest.h"

INC=(-I"$SRC/include" -I"$SRC/src" -I"$SRC/src/util" -I"$GEN")
LIBS=(-lpthread -lm -ldl -lrt)

# 2) librvvm, instrumented: sanitizers + SanCov on the project code, DWARF 3.
make -C "$SRC" -j"$MAYHEM_JOBS" "${RVVM_OPTS[@]}" USE_NO_STACKTRACE=1 \
    BUILDDIR="$OUT/fuzz" \
    CFLAGS="$SANITIZER_FLAGS -fsanitize=fuzzer-no-link $DEBUG_FLAGS" \
    LDFLAGS="$SANITIZER_FLAGS" lib
FUZZLIB="$OUT/fuzz/librvvm_static.a"

$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$SRC/mayhem/lsan_off.c" -o "$OUT/lsan_off.o"
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$OUT/standalone_main.o"

build_harness() { # <name> <source> [defines...]
    local name="$1" src="$2"; shift 2
    $CC $SANITIZER_FLAGS $DEBUG_FLAGS "$@" "${INC[@]}" -c "$src" -o "$OUT/$name.o"
    $CC $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "$OUT/$name.o" "$OUT/lsan_off.o" \
        "$FUZZLIB" "${LIBS[@]}" -o "$SRC/$name"
    $CC $SANITIZER_FLAGS $DEBUG_FLAGS "$OUT/standalone_main.o" "$OUT/$name.o" "$OUT/lsan_off.o" \
        "$FUZZLIB" "${LIBS[@]}" -o "$SRC/$name-standalone"
}
HS="$SRC/mayhem/harnesses"
build_harness fuzz_tap          "$HS/fuzz_tap.c"
build_harness fuzz_guest_jit    "$HS/fuzz_guest.c" -DMODE_CODE -DUSE_JIT_OPT=1
build_harness fuzz_guest_interp "$HS/fuzz_guest.c" -DMODE_CODE -DUSE_JIT_OPT=0
build_harness fuzz_kernel_load  "$HS/fuzz_guest.c" -DMODE_KERNEL
build_harness fuzz_nvme         "$HS/fuzz_guest.c" -DMODE_NVME
build_harness fuzz_mmio_devices "$HS/fuzz_guest.c" -DMODE_DEVICES
build_harness fuzz_snapshot     "$HS/fuzz_guest.c" -DMODE_SNAPSHOT
build_harness fuzz_gdbstub      "$HS/fuzz_gdbstub.c"

# 3) Selftest (functional oracle): librvvm with upstream's normal flags.
env -u CFLAGS -u LDFLAGS make -C "$SRC" -j"$MAYHEM_JOBS" "${RVVM_OPTS[@]}" \
    BUILDDIR="$OUT/release" ${COVERAGE_FLAGS:+CFLAGS="$COVERAGE_FLAGS" LDFLAGS="$COVERAGE_FLAGS"} lib
$CC -O2 $COVERAGE_FLAGS "${INC[@]}" "$SRC/mayhem/selftest/rvvm_selftest.c" \
    "$OUT/release/librvvm_static.a" "${LIBS[@]}" -o "$OUT/rvvm_selftest"

echo "build.sh: done"
