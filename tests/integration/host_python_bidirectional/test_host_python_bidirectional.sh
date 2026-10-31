#!/bin/sh
# #904 contrib.host.python bidirectional embedding: a persistent host-owned VM
# that registers host callbacks the script calls, injects globals, runs
# code, and reads TYPED results back.
#
# Skips cleanly when libpython dev headers / runtime aren't present.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) echo "  [SKIP] host_python_bidirectional on Windows"; exit 0 ;;
esac

if ! command -v python3-config >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then
    echo "  [SKIP] host_python_bidirectional: python3 development files unavailable"
    exit 0
fi

PY_CFLAGS="$(python3-config --embed --cflags 2>/dev/null || python3-config --cflags 2>/dev/null)"
PY_LIBS="$(python3-config --embed --ldflags 2>/dev/null || python3-config --ldflags 2>/dev/null)"
PY_SONAME="$(python3 -c 'import sysconfig; print(sysconfig.get_config_var("INSTSONAME") or "")' 2>/dev/null)"

if [ -z "$PY_CFLAGS" ] || [ -z "$PY_LIBS" ] || [ -z "$PY_SONAME" ]; then
    echo "  [SKIP] host_python_bidirectional: libpython build metadata unavailable"
    exit 0
fi

if [ ! -f "$ROOT/build/libaether.a" ]; then
    echo "  [SKIP] host_python_bidirectional: libaether.a not built"
    exit 0
fi

case "$PY_SONAME" in
    /*) ;;
    *)
        for PY_DIR in \
            "$(python3 -c 'import sysconfig; print(sysconfig.get_config_var("PYTHONFRAMEWORKPREFIX") or "")' 2>/dev/null)" \
            "$(python3 -c 'import sysconfig; print(sysconfig.get_config_var("LIBDIR") or "")' 2>/dev/null)"; do
            if [ -n "$PY_DIR" ] && [ -f "$PY_DIR/$PY_SONAME" ]; then
                PY_SONAME="$PY_DIR/$PY_SONAME"
                break
            fi
        done
        ;;
esac
export AETHER_PYTHON_SONAME="$PY_SONAME"

RT_LIB=""
[ "$(uname -s 2>/dev/null)" = "Linux" ] && RT_LIB="-lrt"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

SOURCE_FILE="${TEST_SOURCE_FILE:-$ROOT/contrib/host/python/aether_host_python.c}"

if ! gcc -o "$TMPDIR/t" \
    "$SOURCE_FILE" \
    "$ROOT/runtime/aether_sandbox.c" \
    "$ROOT/runtime/aether_shared_map.c" \
    $PY_CFLAGS -I"$ROOT" -I"$ROOT/contrib/host/python" -I"$ROOT/runtime" -DAETHER_HAS_PYTHON -DAETHER_HAS_SANDBOX \
    -L"$ROOT/build" -laether $PY_LIBS -ldl -lm $RT_LIB -lpthread \
    -Wno-discarded-qualifiers \
    -xc - 2>"$TMPDIR/cc.log" << 'CEOF'
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>
#include <math.h>

#include "contrib/host/python/aether_host_python.h"

void aether_args_init(int a, char** v){(void)a;(void)v;}
void* _aether_ctx_stack[64]; int _aether_ctx_depth = 0;
extern void* list_new(void); extern void list_add_raw(void*, void*);

static int cb_add(void* vm) {
    int64_t a = python_arg_int(vm, 1);
    int64_t b = python_arg_int(vm, 2);
    python_push_int(vm, a + b);
    return 1;
}

static int cb_echo_str(void* vm) {
    const char* s = python_arg_str(vm, 1);
    python_push_str(vm, s);
    return 1;
}

static int cb_echo_bool(void* vm) {
    int b = python_arg_bool(vm, 1);
    python_push_bool(vm, b);
    return 1;
}

static int cb_echo_nil(void* vm) {
    (void)vm;
    python_push_nil(vm);
    return 1;
}

static int cb_echo_double(void* vm) {
    double d = python_arg_double(vm, 1);
    python_push_double(vm, d * 2.0);
    return 1;
}

static int cb_echo_bytes(void* vm) {
    int len = 0;
    const char* b = python_arg_bytes(vm, 1, &len);
    python_push_bytes(vm, b, len);
    return 1;
}

static int cb_raise_err(void* vm) {
    const char* msg = python_arg_str(vm, 1);
    return python_raise_error(vm, msg);
}

static int cb_nested_eval(void* vm) {
    // Nested eval on the SAME VM which invokes another host callback!
    int rc = python_vm_eval(vm, "app.math.add(2, 4)");
    if (rc != 0) return 0;
    int64_t inner_res = python_vm_result_int(vm);
    python_vm_pop_result(vm);

    // Read argument 1 AFTER the nested eval/callback has executed:
    // If per-call state was not saved and restored across the inner callback,
    // current_args would still be (2, 4) from app.math.add instead of arg1!
    int64_t arg1 = python_arg_int(vm, 1);

    python_push_int(vm, arg1 + inner_res);
    return 1;
}

// Thread worker for multi-threading test
static void* thread_worker(void* arg) {
    void* vm = python_vm_new();
    if (!vm) return (void*)1;

    for (int i = 0; i < 300; i++) {
        int rc = python_vm_eval(vm, "100 + 23");
        if (rc != 0 || python_vm_result_int(vm) != 123) {
            python_vm_free(vm);
            return (void*)2;
        }
        python_vm_pop_result(vm);
    }

    python_vm_free(vm);
    return NULL;
}

static long get_rss_kb(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;
}

int main(void) {
    void* vm = python_vm_new();
    if (!vm) { printf("FAIL: vm_new\n"); return 1; }

    // Register host callbacks (including dotted names)
    python_vm_register(vm, "app.math.add", (void*)cb_add);
    python_vm_register(vm, "host.echo_str", (void*)cb_echo_str);
    python_vm_register(vm, "host.echo_bool", (void*)cb_echo_bool);
    python_vm_register(vm, "host.echo_nil", (void*)cb_echo_nil);
    python_vm_register(vm, "host.echo_double", (void*)cb_echo_double);
    python_vm_register(vm, "host.echo_bytes", (void*)cb_echo_bytes);
    python_vm_register(vm, "host.raise_err", (void*)cb_raise_err);
    python_vm_register(vm, "host.nested_eval", (void*)cb_nested_eval);

    // Globals
    python_vm_set_global_str(vm, "GSTR", "héllo €");
    python_vm_set_global_int(vm, "GINT", 100);
    void* keys = list_new(); list_add_raw(keys, "k1"); list_add_raw(keys, "k2");
    python_vm_set_global_strlist(vm, "KEYS", keys);

    // 1. Persistent VM state & dotted callback (statements give None, globals persist)
    int rc = python_vm_eval(vm, "x = app.math.add(2, 3)\nx += GINT");
    if (rc != 0) { printf("FAIL: eval 1 rc=%d\n", rc); return 1; }
    if (python_vm_result_type(vm) != 0) { // Statements give None
        printf("FAIL: value 1 (type=%d val=%ld)\n", python_vm_result_type(vm), (long)python_vm_result_int(vm));
        return 1;
    }
    python_vm_pop_result(vm);

    // 2. Expression eval sees persisted state 'x'
    rc = python_vm_eval(vm, "x + 5");
    if (rc != 0 || python_vm_result_type(vm) != 2 || python_vm_result_int(vm) != 110) {
        printf("FAIL: eval 2 (rc=%d type=%d val=%ld)\n", rc, python_vm_result_type(vm), (long)python_vm_result_int(vm));
        return 1;
    }
    python_vm_pop_result(vm);

    // 3. Non-ASCII string round trip
    rc = python_vm_eval(vm, "host.echo_str(GSTR)");
    if (rc != 0 || strcmp(python_vm_result_str(vm), "héllo €") != 0) {
        printf("FAIL: non-ascii str (%s)\n", python_vm_result_str(vm)); return 1;
    }
    python_vm_pop_result(vm);

    // 4. Bool & nil & double & strlist
    python_vm_eval(vm, "host.echo_bool(True)");
    if (python_vm_result_type(vm) != 1 || python_vm_result_bool(vm) != 1) {
        printf("FAIL: bool (type=%d, val=%d)\n", python_vm_result_type(vm), python_vm_result_bool(vm));
        return 1;
    }
    python_vm_pop_result(vm);

    python_vm_eval(vm, "host.echo_nil()");
    if (python_vm_result_type(vm) != 0) {
        printf("FAIL: nil (type=%d)\n", python_vm_result_type(vm));
        return 1;
    }
    python_vm_pop_result(vm);

    python_vm_eval(vm, "host.echo_double(2.5)");
    if (python_vm_result_type(vm) != 8 || fabs(python_vm_result_double(vm) - 5.0) > 1e-6) {
        printf("FAIL: double (type=%d, val=%f)\n", python_vm_result_type(vm), python_vm_result_double(vm));
        return 1;
    }
    python_vm_pop_result(vm);

    python_vm_eval(vm, "len(KEYS) == 2 and KEYS[1] == 'k2'");
    if (python_vm_result_type(vm) != 1 || python_vm_result_bool(vm) != 1) {
        printf("FAIL: strlist\n");
        return 1;
    }
    python_vm_pop_result(vm);

    // 5. Bytes with embedded zero
    python_vm_eval(vm, "b'a\\x00b'");
    int b_len = 0;
    const char* b_data = python_vm_result_bytes(vm, &b_len);
    if (python_vm_result_type(vm) != 7 || b_len != 3 || b_data[0] != 'a' || b_data[1] != '\0' || b_data[2] != 'b') {
        printf("FAIL: bytes result (type=%d len=%d)\n", python_vm_result_type(vm), b_len);
        return 1;
    }
    python_vm_pop_result(vm);

    python_vm_eval(vm, "host.echo_bytes(b'x\\x00y')");
    b_len = 0;
    b_data = python_vm_result_bytes(vm, &b_len);
    if (python_vm_result_type(vm) != 7 || b_len != 3 || b_data[0] != 'x' || b_data[1] != '\0' || b_data[2] != 'y') {
        printf("FAIL: bytes cb echo (type=%d len=%d)\n", python_vm_result_type(vm), b_len);
        return 1;
    }
    python_vm_pop_result(vm);

    // 6. Large integer 2**64 overflow -> PY_TOTHER, _int gives 0
    python_vm_eval(vm, "2**64");
    if (python_vm_result_type(vm) != 6 || python_vm_result_int(vm) != 0) {
        printf("FAIL: 2**64 overflow (type=%d int=%ld)\n", python_vm_result_type(vm), (long)python_vm_result_int(vm));
        return 1;
    }
    python_vm_pop_result(vm);

    // 7. List / Dict results -> PY_TTABLE
    python_vm_eval(vm, "[1, 2, 3]");
    if (python_vm_result_type(vm) != 4) {
        printf("FAIL: list table (type=%d)\n", python_vm_result_type(vm));
        return 1;
    }
    python_vm_pop_result(vm);

    python_vm_eval(vm, "{'k': 'v'}");
    if (python_vm_result_type(vm) != 4) {
        printf("FAIL: dict table (type=%d)\n", python_vm_result_type(vm));
        return 1;
    }
    python_vm_pop_result(vm);

    // 8. Statements followed by expression on same VM
    rc = python_vm_eval(vm, "def foo(n):\n    return n * 3\n");
    if (rc != 0) { printf("FAIL: statements def\n"); return 1; }
    python_vm_pop_result(vm);

    rc = python_vm_eval(vm, "foo(14)");
    if (rc != 0 || python_vm_result_int(vm) != 42) {
        printf("FAIL: call foo (rc=%d val=%ld)\n", rc, (long)python_vm_result_int(vm));
        return 1;
    }
    python_vm_pop_result(vm);

    // 9. Exception, then successful eval on same VM
    rc = python_vm_eval(vm, "1 / 0");
    if (rc == 0 || python_vm_result_type(vm) != 5) {
        printf("FAIL: exception type (rc=%d type=%d)\n", rc, python_vm_result_type(vm));
        return 1;
    }
    if (strstr(python_vm_result_str(vm), "ZeroDivisionError") == NULL) {
        printf("FAIL: exception message (%s)\n", python_vm_result_str(vm));
        return 1;
    }
    python_vm_pop_result(vm);

    rc = python_vm_eval(vm, "100 + 200");
    if (rc != 0 || python_vm_result_int(vm) != 300) {
        printf("FAIL: eval after exception (rc=%d val=%ld)\n", rc, (long)python_vm_result_int(vm));
        return 1;
    }
    python_vm_pop_result(vm);

    // 10. raise_error from callback caught by Python try
    rc = python_vm_eval(vm, "try:\n    host.raise_err('deliberate_cb_err')\nexcept RuntimeError as e:\n    err_caught = str(e)\n");
    if (rc != 0) { printf("FAIL: try/catch cb err rc=%d\n", rc); return 1; }
    python_vm_pop_result(vm);

    rc = python_vm_eval(vm, "err_caught");
    if (rc != 0 || strcmp(python_vm_result_str(vm), "deliberate_cb_err") != 0) {
        printf("FAIL: read err_caught (rc=%d str=%s)\n", rc, python_vm_result_str(vm));
        return 1;
    }
    python_vm_pop_result(vm);

    // 11. Nested eval from inside a callback
    rc = python_vm_eval(vm, "host.nested_eval(40)");
    if (rc != 0 || python_vm_result_int(vm) != 46) {
        printf("FAIL: nested eval (rc=%d val=%ld)\n", rc, (long)python_vm_result_int(vm));
        return 1;
    }
    python_vm_pop_result(vm);

    // 12. Two VMs: global set in one is NOT visible in the other
    void* vm2 = python_vm_new();
    python_vm_set_global_str(vm, "VM1_ONLY", "secret");
    python_vm_eval(vm2, "'VM1_ONLY' in globals()");
    if (python_vm_result_bool(vm2) != 0) {
        printf("FAIL: two VMs isolation\n");
        return 1;
    }
    python_vm_pop_result(vm2);
    python_vm_free(vm2);

    // 13. Instruction limit stops infinite loop
    python_vm_set_instruction_limit(vm, 50, 1);
    rc = python_vm_eval(vm, "while True:\n    pass\n");
    if (rc == 0 || python_vm_result_type(vm) != 5) {
        printf("FAIL: instruction limit (rc=%d type=%d)\n", rc, python_vm_result_type(vm));
        return 1;
    }
    python_vm_pop_result(vm);

    // Disarm limit
    python_vm_set_instruction_limit(vm, 0, 0);

    // 14. Two threads evaluating concurrently on different VMs
    pthread_t t1, t2;
    if (pthread_create(&t1, NULL, thread_worker, NULL) != 0 ||
        pthread_create(&t2, NULL, thread_worker, NULL) != 0) {
        printf("FAIL: pthread_create\n");
        return 1;
    }
    void *r1 = NULL, *r2 = NULL;
    pthread_join(t1, &r1);
    pthread_join(t2, &r2);
    if (r1 != NULL || r2 != NULL) {
        printf("FAIL: thread worker failed\n");
        return 1;
    }

    // 15. Leak check: 10,000 rounds
    // sys.getrefcount of an injected object and gc.get_objects() must stay flat
    rc = python_vm_eval(vm,
        "import sys, gc\n"
        "class Tracked:\n"
        "    pass\n"
        "gc.collect()\n"
        "tracked_obj = Tracked()\n"
        "init_ref = sys.getrefcount(tracked_obj)\n"
        "init_objs = len(gc.get_objects())\n");
    if (rc != 0) { printf("FAIL: leak setup\n"); return 1; }
    python_vm_pop_result(vm);

    long rss_before = get_rss_kb();

    for (int i = 0; i < 10000; i++) {
        python_vm_set_global_str(vm, "LOOP_VAR", "loop_val");
        rc = python_vm_eval(vm, "host.echo_str(LOOP_VAR)");
        if (rc != 0) { printf("FAIL: leak loop round %d\n", i); return 1; }
        const char* s = python_vm_result_str(vm);
        (void)s;
        python_vm_pop_result(vm);

        // Also evaluate and read tracked_obj as result to ensure result path refcounts stay flat
        rc = python_vm_eval(vm, "tracked_obj");
        if (rc != 0) { printf("FAIL: leak eval tracked_obj %d\n", i); return 1; }
        python_vm_pop_result(vm);
    }

    long rss_after = get_rss_kb();

    rc = python_vm_eval(vm,
        "gc.collect()\n"
        "final_ref = sys.getrefcount(tracked_obj)\n"
        "final_objs = len(gc.get_objects())\n");
    if (rc != 0) { printf("FAIL: leak collect\n"); return 1; }
    python_vm_pop_result(vm);

    rc = python_vm_eval(vm, "final_ref == init_ref and abs(final_objs - init_objs) < 10");
    if (rc != 0 || python_vm_result_bool(vm) != 1) {
        printf("FAIL: leak check ref/objs not flat\n");
        return 1;
    }
    python_vm_pop_result(vm);

    // RSS growth across 10,000 rounds should be bounded (< 15MB)
    long rss_growth = rss_after - rss_before;
    if (rss_growth > 15 * 1024) {
        printf("FAIL: RSS growth too high (%ld KB)\n", rss_growth);
        return 1;
    }

    python_vm_free(vm);
    printf("PASS: host_python_bidirectional\n");
    return 0;
}
CEOF
then
    echo "  [FAIL] host_python_bidirectional: harness compile/link failed"
    head -8 "$TMPDIR/cc.log" | sed 's/^/      /'
    exit 1
fi

# Wrap execution in a 10-second timeout so a broken instruction limit fails rather than hanging
if command -v timeout >/dev/null 2>&1; then
    OUT="$(timeout 10 "$TMPDIR/t" 2>&1)"
    STATUS=$?
    if [ $STATUS -eq 124 ]; then
        echo "  [FAIL] host_python_bidirectional: test timed out"
        exit 1
    fi
else
    OUT="$("$TMPDIR/t" 2>&1)"
    STATUS=$?
fi

echo "$OUT" | grep -q "PASS: host_python_bidirectional" || { echo "  [FAIL] $OUT"; exit 1; }
echo "  [PASS] host_python_bidirectional: callbacks + typed results + globals + nested + threads + limit + leak check"
