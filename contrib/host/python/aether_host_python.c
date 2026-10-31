// aether_host_python.c — Embedded Python Language Host Module
//
// Embeds CPython in the Aether process. When run_sandboxed is called,
// pushes its grants as a level of the calling thread's (#2955), so Python's libc calls are
// intercepted and checked against the grant list.
//
// LOADING MODEL: dlopen, not -lpython. Resolves libpython at the
// deploy host's runtime via `dlopen("libpython3.so", RTLD_NOW|RTLD_GLOBAL)`,
// with `${AETHER_PYTHON_SONAME}` (e.g. `libpython3.14.so.1.0`) as a
// fallback for hosts where the unversioned symlink isn't present
// (typically the case on Debian unless python3-dev is installed,
// reliably present on Fedora-likes like Bazzite). All Python C API
// references go through dlsym function pointers — the bridge .a
// has NO unresolved CPython symbols at link time, so `ae build`
// produces binaries with no `-lpython` and no DT_NEEDED for libpython,
// making them ABI-portable across the deploy host's Python minor
// version. The compile-anywhere / run-where-Python-is shape.
//
// RTLD_GLOBAL is important: Python C extensions loaded later
// (e.g. ctypes, native modules) must resolve their Py_* references
// against THIS libpython, not pick up a second copy. Without
// RTLD_GLOBAL they get isolated and segfault on shared singletons
// like _Py_NoneStruct.

#include "aether_host_python.h"
#include "../../../runtime/aether_sandbox.h"
#include "../../../runtime/aether_shared_map.h"
#include "../../../std/string/aether_string.h"

#ifdef AETHER_HAS_PYTHON
#include <Python.h>
#include "../aep_dl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AE_PY_TNIL   0
#define AE_PY_TBOOL  1
#define AE_PY_TINT   2
#define AE_PY_TSTR   3
#define AE_PY_TTABLE 4
#define AE_PY_TERR   5
#define AE_PY_TOTHER 6
#define AE_PY_TBYTES 7
#define AE_PY_TFLOAT 8

// --- libpython dlopen table -------------------------------------------------
//
// All CPython C API access goes through this table. Populated by
// load_libpython() on first use; load failure makes every entry point
// return -1 with a clear message rather than segfault.

static void* libpython_handle = NULL;

static struct {
    // Lifecycle.
    void          (*Py_Initialize)(void);
    void          (*Py_Finalize)(void);
    int           (*Py_IsInitialized)(void);
    // Thread & GIL.
    PyThreadState*(*PyEval_SaveThread)(void);
    PyGILState_STATE (*PyGILState_Ensure)(void);
    void          (*PyGILState_Release)(PyGILState_STATE);
    // Refcount helpers.
    void          (*Py_IncRef)(PyObject*);
    void          (*Py_DecRef)(PyObject*);
    // Execution & compilation.
    int           (*PyRun_SimpleStringFlags)(const char*, PyCompilerFlags*);
    PyObject*     (*py_compile_string_ex_flags)(const char*, const char*, int, PyCompilerFlags*, int);
    PyObject*     (*PyEval_EvalCode)(PyObject*, PyObject*, PyObject*);
    // Trace hook for instruction limit.
    void          (*PyEval_SetTrace)(Py_tracefunc, PyObject*);
    // Error handling.
    PyObject*     (*PyErr_Occurred)(void);
    void          (*PyErr_Clear)(void);
    void          (*PyErr_Fetch)(PyObject**, PyObject**, PyObject**);
    void          (*PyErr_NormalizeException)(PyObject**, PyObject**, PyObject**);
    void          (*PyErr_SetString)(PyObject*, const char*);
    // Object inspection & conversion.
    int           (*PyObject_IsInstance)(PyObject*, PyObject*);
    PyObject*     (*PyObject_Str)(PyObject*);
    PyObject*     (*PyObject_GetAttrString)(PyObject*, const char*);
    int           (*PyObject_SetAttrString)(PyObject*, const char*, PyObject*);
    // Numbers.
    long long     (*PyLong_AsLongLongAndOverflow)(PyObject*, int*);
    PyObject*     (*PyLong_FromLongLong)(long long);
    double        (*PyFloat_AsDouble)(PyObject*);
    PyObject*     (*PyFloat_FromDouble)(double);
    PyObject*     (*PyBool_FromLong)(long);
    // Strings & Bytes.
    PyObject*     (*PyUnicode_FromString)(const char*);
    PyObject*     (*PyUnicode_FromStringAndSize)(const char*, Py_ssize_t);
    const char*   (*PyUnicode_AsUTF8AndSize)(PyObject*, Py_ssize_t*);
    PyObject*     (*PyBytes_FromStringAndSize)(const char*, Py_ssize_t);
    int           (*PyBytes_AsStringAndSize)(PyObject*, char**, Py_ssize_t*);
    // Collections & Tuples.
    PyObject*     (*PyList_New)(Py_ssize_t);
    int           (*PyList_Append)(PyObject*, PyObject*);
    Py_ssize_t    (*PyList_Size)(PyObject*);
    PyObject*     (*PyList_GetItem)(PyObject*, Py_ssize_t);
    Py_ssize_t    (*PyTuple_Size)(PyObject*);
    PyObject*     (*PyTuple_GetItem)(PyObject*, Py_ssize_t);
    // Args parsing — varargs; dlsym is fine with varargs sigs.
    int           (*PyArg_ParseTuple)(PyObject*, const char*, ...);
    // Module / dict / capsules.
    PyObject*     (*PyModule_New)(const char*);
    PyObject*     (*PyModule_Create2)(PyModuleDef*, int);
    PyObject*     (*PyImport_GetModuleDict)(void);
    PyObject*     (*PyEval_GetBuiltins)(void);
    PyObject*     (*PyDict_New)(void);
    int           (*PyDict_SetItemString)(PyObject*, const char*, PyObject*);
    PyObject*     (*PyDict_GetItemString)(PyObject*, const char*);
    PyObject*     (*py_cfunction_new_ex)(PyMethodDef*, PyObject*, PyObject*);
    PyObject*     (*PyCapsule_New)(void*, const char*, PyCapsule_Destructor);
    void*         (*PyCapsule_GetPointer)(PyObject*, const char*);

    // Data symbols (addresses of type singletons / exceptions / None).
    PyObject*     py_none;
    PyObject*     py_bool_type;
    PyObject*     py_long_type;
    PyObject*     py_float_type;
    PyObject*     py_unicode_type;
    PyObject*     py_bytes_type;
    PyObject*     py_list_type;
    PyObject*     py_dict_type;
    PyObject*     py_exc_runtime_error;
} g_py;

// Try one library name; return non-NULL handle on success.
static void* try_dlopen(const char* name) {
    if (!name || !*name) return NULL;
    return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}

// Populate g_py from a loaded libpython handle. Returns 0 on success,
// -1 if any required symbol is missing.
static int resolve_python_symbols(void* h) {
#define RESOLVE(field, sym) do {                                      \
        *(void**)(&g_py.field) = dlsym(h, sym);                       \
        if (!g_py.field) {                                            \
            fprintf(stderr,                                           \
                "aether host_python: libpython missing symbol %s\n",  \
                sym);                                                 \
            return -1;                                                \
        }                                                             \
    } while (0)

    RESOLVE(Py_Initialize,               "Py_Initialize");
    RESOLVE(Py_Finalize,                 "Py_Finalize");
    RESOLVE(Py_IsInitialized,            "Py_IsInitialized");
    RESOLVE(PyEval_SaveThread,           "PyEval_SaveThread");
    RESOLVE(PyGILState_Ensure,           "PyGILState_Ensure");
    RESOLVE(PyGILState_Release,          "PyGILState_Release");
    RESOLVE(Py_IncRef,                   "Py_IncRef");
    RESOLVE(Py_DecRef,                   "Py_DecRef");
    RESOLVE(PyRun_SimpleStringFlags,     "PyRun_SimpleStringFlags");
    RESOLVE(py_compile_string_ex_flags,  "Py_CompileStringExFlags");
    RESOLVE(PyEval_EvalCode,             "PyEval_EvalCode");
    RESOLVE(PyEval_SetTrace,             "PyEval_SetTrace");
    RESOLVE(PyErr_Occurred,              "PyErr_Occurred");
    RESOLVE(PyErr_Clear,                 "PyErr_Clear");
    RESOLVE(PyErr_Fetch,                 "PyErr_Fetch");
    RESOLVE(PyErr_NormalizeException,    "PyErr_NormalizeException");
    RESOLVE(PyErr_SetString,             "PyErr_SetString");
    RESOLVE(PyObject_IsInstance,         "PyObject_IsInstance");
    RESOLVE(PyObject_Str,                "PyObject_Str");
    RESOLVE(PyObject_GetAttrString,      "PyObject_GetAttrString");
    RESOLVE(PyObject_SetAttrString,      "PyObject_SetAttrString");
    RESOLVE(PyLong_AsLongLongAndOverflow,"PyLong_AsLongLongAndOverflow");
    RESOLVE(PyLong_FromLongLong,         "PyLong_FromLongLong");
    RESOLVE(PyFloat_AsDouble,            "PyFloat_AsDouble");
    RESOLVE(PyFloat_FromDouble,          "PyFloat_FromDouble");
    RESOLVE(PyBool_FromLong,             "PyBool_FromLong");
    RESOLVE(PyUnicode_FromString,        "PyUnicode_FromString");
    RESOLVE(PyUnicode_FromStringAndSize, "PyUnicode_FromStringAndSize");
    RESOLVE(PyUnicode_AsUTF8AndSize,     "PyUnicode_AsUTF8AndSize");
    RESOLVE(PyBytes_FromStringAndSize,   "PyBytes_FromStringAndSize");
    RESOLVE(PyBytes_AsStringAndSize,     "PyBytes_AsStringAndSize");
    RESOLVE(PyList_New,                  "PyList_New");
    RESOLVE(PyList_Append,               "PyList_Append");
    RESOLVE(PyList_Size,                 "PyList_Size");
    RESOLVE(PyList_GetItem,              "PyList_GetItem");
    RESOLVE(PyTuple_Size,                "PyTuple_Size");
    RESOLVE(PyTuple_GetItem,             "PyTuple_GetItem");
    RESOLVE(PyArg_ParseTuple,            "PyArg_ParseTuple");
    RESOLVE(PyModule_New,                "PyModule_New");
    RESOLVE(PyModule_Create2,            "PyModule_Create2");
    RESOLVE(PyImport_GetModuleDict,      "PyImport_GetModuleDict");
    RESOLVE(PyEval_GetBuiltins,          "PyEval_GetBuiltins");
    RESOLVE(PyDict_New,                  "PyDict_New");
    RESOLVE(PyDict_SetItemString,        "PyDict_SetItemString");
    RESOLVE(PyDict_GetItemString,        "PyDict_GetItemString");
    RESOLVE(py_cfunction_new_ex,         "PyCFunction_NewEx");
    RESOLVE(PyCapsule_New,               "PyCapsule_New");
    RESOLVE(PyCapsule_GetPointer,        "PyCapsule_GetPointer");

    // DATA symbols:
    g_py.py_none = (PyObject*)dlsym(h, "_Py_NoneStruct");
    if (!g_py.py_none) {
        fprintf(stderr, "aether host_python: libpython missing _Py_NoneStruct\n");
        return -1;
    }

    g_py.py_bool_type = (PyObject*)dlsym(h, "PyBool_Type");
    g_py.py_long_type = (PyObject*)dlsym(h, "PyLong_Type");
    g_py.py_float_type = (PyObject*)dlsym(h, "PyFloat_Type");
    g_py.py_unicode_type = (PyObject*)dlsym(h, "PyUnicode_Type");
    g_py.py_bytes_type = (PyObject*)dlsym(h, "PyBytes_Type");
    g_py.py_list_type = (PyObject*)dlsym(h, "PyList_Type");
    g_py.py_dict_type = (PyObject*)dlsym(h, "PyDict_Type");

    PyObject** exc_rt = (PyObject**)dlsym(h, "PyExc_RuntimeError");
    if (exc_rt) {
        g_py.py_exc_runtime_error = *exc_rt;
    }

    if (!g_py.py_bool_type || !g_py.py_long_type || !g_py.py_float_type ||
        !g_py.py_unicode_type || !g_py.py_bytes_type || !g_py.py_list_type ||
        !g_py.py_dict_type || !g_py.py_exc_runtime_error) {
        fprintf(stderr, "aether host_python: libpython missing required type/exception symbol\n");
        return -1;
    }

    return 0;
#undef RESOLVE
}

// Load libpython on first use. Strict two-step contract:
//   1. ${AETHER_PYTHON_SONAME} env var (orchestrator-supplied exact
//      soname, e.g. "libpython3.14.so.1.0"). The orchestrator
//      (aeb-ctr or equivalent) should probe the host's python via
//      sysconfig and pass this; aeb-ctr does so.
//   2. "libpython3.so" (unversioned symlink, present in the runtime
//      package on Fedora-likes; -dev-only on Debian-likes).
// No versioned fallback list — hardcoding `libpython3.X.so.1.0`
// would be a maintenance treadmill (new minor → silent miss → looks
// like a runtime bug). If both fail, error out clearly so the
// orchestrator gets a real signal.
// Returns 0 on success, -1 on any failure.
static int load_libpython(void) {
    if (libpython_handle) return 0;

    const char* env_soname = getenv("AETHER_PYTHON_SONAME");
    void* h = try_dlopen(env_soname);
    if (!h) h = try_dlopen("libpython3.so");
    if (!h) {
        fprintf(stderr,
            "aether host_python: cannot dlopen libpython "
            "(tried $AETHER_PYTHON_SONAME=%s, libpython3.so).\n"
            "  Install a python3 runtime on the host, or set "
            "AETHER_PYTHON_SONAME to the exact soname "
            "(e.g. libpython3.12.so.1.0).\n"
            "  Hint: $(python3 -c 'import sysconfig; "
            "print(sysconfig.get_config_var(\"INSTSONAME\"))')\n"
            "  dlerror: %s\n",
            env_soname ? env_soname : "(unset)",
            dlerror() ? dlerror() : "(none)");
        return -1;
    }

    if (resolve_python_symbols(h) != 0) {
        dlclose(h);
        return -1;
    }

    libpython_handle = h;
    return 0;
}

// --- sandbox / permission stack (bridge-local; unchanged from pre-dlopen) ---

static int python_initialized = 0;
static PyThreadState* g_saved_thread_state = NULL;

extern int list_size(void*);
extern void* list_get_raw(void*, int);
/* A grant list std.sandbox built holds plain C strings; one a program
 * built can hold Aether strings (a helper that keeps its `string`
 * parameter stores its own copy). This reads the bytes of either. */
extern const char* aether_string_data(const void*);

// The shared grant matcher (runtime/aether_sandbox_match.h).
#include "aether_sandbox_match.h"

int python_init(void) {
    if (python_initialized) return 0;
    if (load_libpython() != 0) return -1;
    if (!g_py.Py_IsInitialized()) {
        g_py.Py_Initialize();
        // Release the GIL on initialization so subsequent threads can use PyGILState_Ensure
        g_saved_thread_state = g_py.PyEval_SaveThread();
    }
    python_initialized = 1;
    return 0;
}

void python_finalize(void) {
    if (python_initialized) {
        if (g_saved_thread_state) {
            PyGILState_STATE gstate = g_py.PyGILState_Ensure();
            (void)gstate;
            g_py.Py_Finalize();
            g_saved_thread_state = NULL;
        } else {
            g_py.Py_Finalize();
        }
        python_initialized = 0;
    }
}

int python_run(const char* code) {
    if (!code) return -1;
    if (python_init() != 0) return -1;
    PyGILState_STATE gstate = g_py.PyGILState_Ensure();
    int rc = g_py.PyRun_SimpleStringFlags(code, NULL);
    g_py.PyGILState_Release(gstate);
    return rc;
}

int python_run_sandboxed(void* perms, const char* code) {
    if (!code) return -1;
    if (python_init() != 0) return -1;
    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) return -1;

    // The grants as a level of this thread's, in the runtime (#2955)
    aether_sandbox_push(perms);

    PyGILState_STATE gstate = g_py.PyGILState_Ensure();
    int result = g_py.PyRun_SimpleStringFlags(code, NULL);
    g_py.PyGILState_Release(gstate);

    // Restore
    aether_sandbox_pop();

    return result;
}

// --- Shared map bindings for Python -----------------------------------------

static uint64_t py_current_map_token = 0;

// Python callable: aether_map_get(key) → str or None
static PyObject* py_aether_map_get(PyObject* self, PyObject* args) {
    (void)self;
    const char* key;
    if (!g_py.PyArg_ParseTuple(args, "s", &key)) return NULL;
    const char* val = aether_shared_map_get_by_token(py_current_map_token, key);
    if (val) return g_py.PyUnicode_FromString(val);
    // Py_RETURN_NONE equivalent: incref None and return it.
    g_py.Py_IncRef(g_py.py_none);
    return g_py.py_none;
}

// Python callable: aether_map_put(key, value)
static PyObject* py_aether_map_put(PyObject* self, PyObject* args) {
    (void)self;
    const char* key;
    const char* value;
    if (!g_py.PyArg_ParseTuple(args, "ss", &key, &value)) return NULL;
    aether_shared_map_put_by_token(py_current_map_token, key, value);
    g_py.Py_IncRef(g_py.py_none);
    return g_py.py_none;
}

static PyMethodDef aether_map_methods[] = {
    {"aether_map_get", py_aether_map_get, METH_VARARGS, "Get value from Aether shared map"},
    {"aether_map_put", py_aether_map_put, METH_VARARGS, "Put value to Aether shared map"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef aether_map_module = {
    PyModuleDef_HEAD_INIT, "_aether_map", NULL, -1, aether_map_methods,
    NULL, NULL, NULL, NULL
};

static int map_module_registered = 0;

static void register_map_module(void) {
    if (map_module_registered) return;
    // PyModule_Create(&def) is a macro that calls
    // PyModule_Create2(&def, PYTHON_API_VERSION). We dlsym the latter
    // directly. PYTHON_API_VERSION is from the headers we compiled
    // against; ABI-compatible with the host's libpython for the
    // module-init handshake (the value has been stable across recent
    // CPython releases; the version it cares about is the one in
    // PyModuleDef_HEAD_INIT).
    PyObject* mod = g_py.PyModule_Create2(&aether_map_module, PYTHON_API_VERSION);
    if (mod) {
        PyObject* sys_modules = g_py.PyImport_GetModuleDict();
        g_py.PyDict_SetItemString(sys_modules, "_aether_map", mod);
        g_py.Py_DecRef(mod);
    }
    map_module_registered = 1;
}

int python_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token) {
    if (!code) return -1;
    if (python_init() != 0) return -1;

    PyGILState_STATE gstate = g_py.PyGILState_Ensure();
    register_map_module();

    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) {
        g_py.PyGILState_Release(gstate);
        return -1;
    }

    // Freeze inputs and set active token
    extern void aether_shared_map_freeze_inputs_by_token(uint64_t);
    aether_shared_map_freeze_inputs_by_token(map_token);
    uint64_t previous_map_token = py_current_map_token;
    py_current_map_token = map_token;

    // Inject convenience imports so user code just calls aether_map_get/put
    const char* preamble =
        "from _aether_map import aether_map_get, aether_map_put\n";

    // The grants as a level of this thread's, in the runtime (#2955)
    aether_sandbox_push(perms);

    // Run preamble + user code
    g_py.PyRun_SimpleStringFlags(preamble, NULL);
    int result = g_py.PyRun_SimpleStringFlags(code, NULL);

    // Restore
    aether_sandbox_pop();
    py_current_map_token = previous_map_token;
    g_py.PyGILState_Release(gstate);

    return result;
}

// --- Tier 2: Bidirectional Embedding API (#904) -----------------------------

typedef struct AetherPythonVM {
    // Globals dict for this VM (has __builtins__ injected).
    PyObject* globals;

    // Result of the last eval (owned by VM until pop or new eval).
    PyObject* result_obj;
    int       result_type;
    int64_t   result_int;
    double    result_double;
    char*     result_str;       // NUL-terminated for result_str, dynamically allocated
    char*     result_bytes;     // Binary data for result_bytes, dynamically allocated
    int       result_bytes_len;

    // Per-call state (for callback execution; saved & restored on nested evals).
    PyObject* current_args;     // Borrowed from CFunction arguments
    PyObject* pushed_result;    // Owned new ref created by python_push_*
    int       raise_error_flag;
    char*     raise_error_msg;

    // Instruction limit.
    int64_t   instruction_budget;
    int64_t   instruction_counter;
    int       instruction_tripped;

    // Allocations linked list for cleanup upon vm_free.
    void*     alloc_head;
} AetherPythonVM;

// Linked list of heap allocations owned by VM (method defs, capsules, etc.).
typedef struct VMAllocNode {
    struct VMAllocNode* next;
    void* ptr;
} VMAllocNode;

static void vm_track_alloc(AetherPythonVM* vm, void* p) {
    if (!vm || !p) return;
    VMAllocNode* node = (VMAllocNode*)malloc(sizeof(VMAllocNode));
    if (!node) return;
    node->ptr = p;
    node->next = (VMAllocNode*)vm->alloc_head;
    vm->alloc_head = node;
}

typedef struct HostCallbackData {
    AetherPythonVM* vm;
    int (*host_fn)(void*);
} HostCallbackData;

static void clear_vm_result(AetherPythonVM* vm) {
    if (!vm) return;
    if (vm->result_obj) {
        g_py.Py_DecRef(vm->result_obj);
        vm->result_obj = NULL;
    }
    if (vm->result_str) {
        free(vm->result_str);
        vm->result_str = NULL;
    }
    if (vm->result_bytes) {
        free(vm->result_bytes);
        vm->result_bytes = NULL;
    }
    vm->result_bytes_len = 0;
    vm->result_type = AE_PY_TNIL;
    vm->result_int = 0;
    vm->result_double = 0.0;
}

void* python_vm_new(void) {
    if (python_init() != 0) return NULL;

    PyGILState_STATE gstate = g_py.PyGILState_Ensure();

    AetherPythonVM* vm = (AetherPythonVM*)calloc(1, sizeof(AetherPythonVM));
    if (!vm) {
        g_py.PyGILState_Release(gstate);
        return NULL;
    }

    vm->globals = g_py.PyDict_New();
    if (!vm->globals) {
        free(vm);
        g_py.PyGILState_Release(gstate);
        return NULL;
    }

    // Set __builtins__ on VM's globals dict
    PyObject* builtins = g_py.PyEval_GetBuiltins();
    if (builtins) {
        g_py.PyDict_SetItemString(vm->globals, "__builtins__", builtins);
    }

    vm->result_type = AE_PY_TNIL;

    g_py.PyGILState_Release(gstate);
    return (void*)vm;
}

void python_vm_free(void* v) {
    if (!v) return;
    AetherPythonVM* vm = (AetherPythonVM*)v;

    PyGILState_STATE gstate = g_py.PyGILState_Ensure();

    clear_vm_result(vm);

    if (vm->pushed_result) {
        g_py.Py_DecRef(vm->pushed_result);
        vm->pushed_result = NULL;
    }
    if (vm->raise_error_msg) {
        free(vm->raise_error_msg);
        vm->raise_error_msg = NULL;
    }
    if (vm->globals) {
        g_py.Py_DecRef(vm->globals);
        vm->globals = NULL;
    }

    // Free tracked allocations
    VMAllocNode* curr = (VMAllocNode*)vm->alloc_head;
    while (curr) {
        VMAllocNode* next = curr->next;
        free(curr->ptr);
        free(curr);
        curr = next;
    }

    free(vm);

    g_py.PyGILState_Release(gstate);
}

// Trampoline called by CPython when a registered host callback is invoked from Python script.
static PyObject* python_host_cb_trampoline(PyObject* self, PyObject* args) {
    HostCallbackData* cd = (HostCallbackData*)g_py.PyCapsule_GetPointer(self, "aether_host_cb");
    if (!cd || !cd->vm || !cd->host_fn) {
        g_py.Py_IncRef(g_py.py_none);
        return g_py.py_none;
    }

    AetherPythonVM* vm = cd->vm;

    // Save previous per-call state for nested calls support
    PyObject* prev_args = vm->current_args;
    PyObject* prev_pushed = vm->pushed_result;
    int       prev_raise_flag = vm->raise_error_flag;
    char*     prev_raise_msg = vm->raise_error_msg;

    vm->current_args = args;
    vm->pushed_result = NULL;
    vm->raise_error_flag = 0;
    vm->raise_error_msg = NULL;

    // Invoke Aether callback
    int rc = cd->host_fn((void*)vm);

    PyObject* ret = NULL;
    if (vm->raise_error_flag) {
        g_py.PyErr_SetString(g_py.py_exc_runtime_error,
                             vm->raise_error_msg ? vm->raise_error_msg : "error");
        ret = NULL; // signals Python exception
    } else if (rc > 0 && vm->pushed_result) {
        ret = vm->pushed_result; // transfer ownership to Python runtime
        vm->pushed_result = NULL;
    } else {
        g_py.Py_IncRef(g_py.py_none);
        ret = g_py.py_none;
    }

    if (vm->raise_error_msg) {
        free(vm->raise_error_msg);
        vm->raise_error_msg = NULL;
    }
    if (vm->pushed_result) {
        g_py.Py_DecRef(vm->pushed_result);
        vm->pushed_result = NULL;
    }

    // Restore previous per-call state
    vm->current_args = prev_args;
    vm->pushed_result = prev_pushed;
    vm->raise_error_flag = prev_raise_flag;
    vm->raise_error_msg = prev_raise_msg;

    return ret;
}

int python_vm_register(void* v, const char* name, void* host_fn) {
    if (!v || !name || !host_fn) return -1;
    AetherPythonVM* vm = (AetherPythonVM*)v;

    PyGILState_STATE gstate = g_py.PyGILState_Ensure();

    char tmp[256];
    size_t name_len = strlen(name);
    if (name_len >= sizeof(tmp)) {
        g_py.PyGILState_Release(gstate);
        return -1;
    }
    memcpy(tmp, name, name_len + 1);

    char* curr = tmp;
    char* dot = strchr(curr, '.');

    // Start with root globals dict
    PyObject* current_obj = NULL;

    if (dot) {
        *dot = '\0';
        current_obj = g_py.PyDict_GetItemString(vm->globals, curr);
        if (!current_obj) {
            current_obj = g_py.PyModule_New(curr);
            g_py.PyDict_SetItemString(vm->globals, curr, current_obj);
            g_py.Py_DecRef(current_obj); // PyDict_SetItemString incref'd it
        }
        curr = dot + 1;
        dot = strchr(curr, '.');

        while (dot) {
            *dot = '\0';
            PyObject* next_obj = g_py.PyObject_GetAttrString(current_obj, curr);
            if (!next_obj) {
                g_py.PyErr_Clear();
                next_obj = g_py.PyModule_New(curr);
                g_py.PyObject_SetAttrString(current_obj, curr, next_obj);
                g_py.Py_DecRef(next_obj); // PyObject_SetAttrString incref'd it
            } else {
                g_py.Py_DecRef(next_obj); // GetAttrString returned new ref
            }
            current_obj = next_obj;
            curr = dot + 1;
            dot = strchr(curr, '.');
        }
    }

    // Allocate callback metadata
    HostCallbackData* cd = (HostCallbackData*)malloc(sizeof(HostCallbackData));
    if (!cd) {
        g_py.PyGILState_Release(gstate);
        return -1;
    }
    cd->vm = vm;
    cd->host_fn = (int (*)(void*))host_fn;
    vm_track_alloc(vm, cd);

    PyObject* capsule = g_py.PyCapsule_New(cd, "aether_host_cb", NULL);
    if (!capsule) {
        g_py.PyGILState_Release(gstate);
        return -1;
    }

    PyMethodDef* md = (PyMethodDef*)calloc(1, sizeof(PyMethodDef));
    if (!md) {
        g_py.Py_DecRef(capsule);
        g_py.PyGILState_Release(gstate);
        return -1;
    }
    md->ml_name = strdup(curr);
    md->ml_meth = (PyCFunction)python_host_cb_trampoline;
    md->ml_flags = METH_VARARGS;
    vm_track_alloc(vm, (void*)md->ml_name);
    vm_track_alloc(vm, md);

    PyObject* fn_obj = g_py.py_cfunction_new_ex(md, capsule, NULL);
    g_py.Py_DecRef(capsule); // CFunction keeps its own reference to self

    if (!fn_obj) {
        g_py.PyGILState_Release(gstate);
        return -1;
    }

    if (current_obj) {
        g_py.PyObject_SetAttrString(current_obj, curr, fn_obj);
    } else {
        g_py.PyDict_SetItemString(vm->globals, curr, fn_obj);
    }

    g_py.Py_DecRef(fn_obj); // Dictionary / object attribute keeps the reference

    g_py.PyGILState_Release(gstate);
    return 0;
}

void python_vm_set_global_str(void* v, const char* name, const char* value) {
    if (!v || !name) return;
    AetherPythonVM* vm = (AetherPythonVM*)v;

    PyGILState_STATE gstate = g_py.PyGILState_Ensure();
    PyObject* str_obj = g_py.PyUnicode_FromString(value ? value : "");
    if (str_obj) {
        g_py.PyDict_SetItemString(vm->globals, name, str_obj);
        g_py.Py_DecRef(str_obj); // Dict keeps reference
    }
    g_py.PyGILState_Release(gstate);
}

void python_vm_set_global_int(void* v, const char* name, int64_t value) {
    if (!v || !name) return;
    AetherPythonVM* vm = (AetherPythonVM*)v;

    PyGILState_STATE gstate = g_py.PyGILState_Ensure();
    PyObject* int_obj = g_py.PyLong_FromLongLong(value);
    if (int_obj) {
        g_py.PyDict_SetItemString(vm->globals, name, int_obj);
        g_py.Py_DecRef(int_obj); // Dict keeps reference
    }
    g_py.PyGILState_Release(gstate);
}

void python_vm_set_global_strlist(void* v, const char* name, void* items) {
    if (!v || !name) return;
    AetherPythonVM* vm = (AetherPythonVM*)v;

    PyGILState_STATE gstate = g_py.PyGILState_Ensure();

    int count = items ? list_size(items) : 0;
    PyObject* list_obj = g_py.PyList_New(0);
    if (list_obj) {
        for (int i = 0; i < count; i++) {
            void* item = list_get_raw(items, i);
            const char* str = item ? aether_string_data(item) : "";
            PyObject* s = g_py.PyUnicode_FromString(str ? str : "");
            if (s) {
                g_py.PyList_Append(list_obj, s);
                g_py.Py_DecRef(s); // Append incref'd it
            }
        }
        g_py.PyDict_SetItemString(vm->globals, name, list_obj);
        g_py.Py_DecRef(list_obj); // Dict keeps reference
    }

    g_py.PyGILState_Release(gstate);
}

// Instruction limit trace hook: decrements budget and raises RuntimeError on expiry.
// Re-raises continuously once tripped so script try/catch blocks cannot simply resume.
static int python_trace_hook(PyObject* obj, PyFrameObject* frame, int what, PyObject* arg) {
    (void)frame; (void)what; (void)arg;
    AetherPythonVM* vm = (AetherPythonVM*)g_py.PyCapsule_GetPointer(obj, "vm_trace");
    if (!vm) return 0;

    if (vm->instruction_tripped) {
        g_py.PyErr_SetString(g_py.py_exc_runtime_error, "instruction limit exceeded");
        return -1;
    }

    if (vm->instruction_budget > 0) {
        vm->instruction_counter--;
        if (vm->instruction_counter <= 0) {
            vm->instruction_tripped = 1;
            g_py.PyErr_SetString(g_py.py_exc_runtime_error, "instruction limit exceeded");
            return -1;
        }
    }
    return 0;
}

static void inspect_py_object(AetherPythonVM* vm, PyObject* obj) {
    if (!obj || obj == g_py.py_none) {
        vm->result_type = AE_PY_TNIL;
        return;
    }

    // 1. Check bool BEFORE int (Python's bool is a subclass of int!)
    if (g_py.PyObject_IsInstance(obj, g_py.py_bool_type)) {
        vm->result_type = AE_PY_TBOOL;
        // PyLong_AsLongLongAndOverflow works for bool too
        int ov = 0;
        vm->result_int = g_py.PyLong_AsLongLongAndOverflow(obj, &ov);
        return;
    }

    // 2. Check int (long) with overflow check for 64-bit integers
    if (g_py.PyObject_IsInstance(obj, g_py.py_long_type)) {
        int overflow = 0;
        long long val = g_py.PyLong_AsLongLongAndOverflow(obj, &overflow);
        if (overflow != 0) {
            vm->result_type = AE_PY_TOTHER;
            vm->result_int = 0; // Documented: reading with _int gives 0 when overflowing
        } else {
            vm->result_type = AE_PY_TINT;
            vm->result_int = (int64_t)val;
        }
        return;
    }

    // 3. Check float
    if (g_py.PyObject_IsInstance(obj, g_py.py_float_type)) {
        vm->result_type = AE_PY_TFLOAT;
        vm->result_double = g_py.PyFloat_AsDouble(obj);
        return;
    }

    // 4. Check bytes (binary-safe)
    if (g_py.PyObject_IsInstance(obj, g_py.py_bytes_type)) {
        vm->result_type = AE_PY_TBYTES;
        char* buf = NULL;
        Py_ssize_t len = 0;
        if (g_py.PyBytes_AsStringAndSize(obj, &buf, &len) == 0 && buf) {
            vm->result_bytes = (char*)malloc((size_t)len + 1);
            if (vm->result_bytes) {
                memcpy(vm->result_bytes, buf, (size_t)len);
                vm->result_bytes[len] = '\0';
                vm->result_bytes_len = (int)len;
            }
        }
        return;
    }

    // 5. Check string (unicode)
    if (g_py.PyObject_IsInstance(obj, g_py.py_unicode_type)) {
        vm->result_type = AE_PY_TSTR;
        Py_ssize_t len = 0;
        const char* utf8 = g_py.PyUnicode_AsUTF8AndSize(obj, &len);
        if (utf8) {
            vm->result_str = (char*)malloc((size_t)len + 1);
            if (vm->result_str) {
                memcpy(vm->result_str, utf8, (size_t)len + 1);
            }
        }
        return;
    }

    // 6. Check collections (list / dict) -> PY_TTABLE
    if (g_py.PyObject_IsInstance(obj, g_py.py_list_type) ||
        g_py.PyObject_IsInstance(obj, g_py.py_dict_type)) {
        vm->result_type = AE_PY_TTABLE;
        return;
    }

    // 7. Otherwise PY_TOTHER
    vm->result_type = AE_PY_TOTHER;
}

int python_vm_eval(void* v, const char* code) {
    if (!v || !code) return -1;
    AetherPythonVM* vm = (AetherPythonVM*)v;

    PyGILState_STATE gstate = g_py.PyGILState_Ensure();

    clear_vm_result(vm);

    // Install trace hook if instruction limit armed
    PyObject* trace_cap = NULL;
    if (vm->instruction_budget > 0) {
        vm->instruction_counter = vm->instruction_budget;
        vm->instruction_tripped = 0;
        trace_cap = g_py.PyCapsule_New(vm, "vm_trace", NULL);
        if (trace_cap) {
            g_py.PyEval_SetTrace(python_trace_hook, trace_cap);
        }
    }

    // First attempt to compile as expression (Py_eval_input).
    // If SyntaxError, compile again as file statements (Py_file_input).
    // Only then execute once.
    PyObject* code_obj = g_py.py_compile_string_ex_flags(code, "<eval>", Py_eval_input, NULL, -1);
    if (!code_obj) {
        g_py.PyErr_Clear();
        code_obj = g_py.py_compile_string_ex_flags(code, "<eval>", Py_file_input, NULL, -1);
    }

    int rc = 0;
    if (!code_obj) {
        // SyntaxError during compilation
        rc = -1;
    } else {
        PyObject* eval_res = g_py.PyEval_EvalCode(code_obj, vm->globals, vm->globals);
        g_py.Py_DecRef(code_obj); // Decref compiled code object

        if (!eval_res) {
            rc = -1;
        } else {
            vm->result_obj = eval_res; // VM keeps reference
            inspect_py_object(vm, eval_res);
        }
    }

    // Remove trace hook
    if (trace_cap) {
        g_py.PyEval_SetTrace(NULL, NULL);
        g_py.Py_DecRef(trace_cap);
    }

    // If an exception occurred:
    if (rc != 0 || g_py.PyErr_Occurred()) {
        rc = -1;
        vm->result_type = AE_PY_TERR;

        PyObject *ptype = NULL, *pval = NULL, *ptrace = NULL;
        g_py.PyErr_Fetch(&ptype, &pval, &ptrace);
        if (ptype) {
            g_py.PyErr_NormalizeException(&ptype, &pval, &ptrace);

            const char* type_name = "Error";
            PyObject* name_obj = g_py.PyObject_GetAttrString(ptype, "__name__");
            if (name_obj) {
                const char* s = g_py.PyUnicode_AsUTF8AndSize(name_obj, NULL);
                if (s) type_name = s;
            }

            const char* msg = "";
            PyObject* str_val = pval ? g_py.PyObject_Str(pval) : NULL;
            if (str_val) {
                const char* s = g_py.PyUnicode_AsUTF8AndSize(str_val, NULL);
                if (s) msg = s;
            }

            char errbuf[512];
            snprintf(errbuf, sizeof(errbuf), "%s: %s", type_name, msg);
            vm->result_str = strdup(errbuf);

            if (name_obj) g_py.Py_DecRef(name_obj);
            if (str_val) g_py.Py_DecRef(str_val);
            g_py.Py_DecRef(ptype);
            if (pval) g_py.Py_DecRef(pval);
            if (ptrace) g_py.Py_DecRef(ptrace);
        } else {
            vm->result_str = strdup("Error: unknown");
        }

        g_py.PyErr_Clear(); // Always clear error indicator for clean subsequent calls
    }

    g_py.PyGILState_Release(gstate);
    return rc;
}

int python_vm_result_type(void* v) {
    if (!v) return AE_PY_TNIL;
    return ((AetherPythonVM*)v)->result_type;
}

int64_t python_vm_result_int(void* v) {
    if (!v) return 0;
    return ((AetherPythonVM*)v)->result_int;
}

const char* python_vm_result_str(void* v) {
    if (!v) return "";
    AetherPythonVM* vm = (AetherPythonVM*)v;
    if (vm->result_str) return vm->result_str;
    if (vm->result_type == AE_PY_TNIL) return "None";
    return "";
}

int python_vm_result_bool(void* v) {
    if (!v) return 0;
    return ((AetherPythonVM*)v)->result_int != 0 ? 1 : 0;
}

double python_vm_result_double(void* v) {
    if (!v) return 0.0;
    return ((AetherPythonVM*)v)->result_double;
}

const char* python_vm_result_bytes(void* v, int* out_len) {
    if (!v) {
        if (out_len) *out_len = 0;
        return "";
    }
    AetherPythonVM* vm = (AetherPythonVM*)v;
    if (out_len) *out_len = vm->result_bytes_len;
    return vm->result_bytes ? vm->result_bytes : "";
}

void python_vm_pop_result(void* v) {
    if (!v) return;
    AetherPythonVM* vm = (AetherPythonVM*)v;
    PyGILState_STATE gstate = g_py.PyGILState_Ensure();
    clear_vm_result(vm);
    g_py.PyGILState_Release(gstate);
}

const char* python_vm_set_instruction_limit(void* v, int64_t budget, int every) {
    (void)every;
    if (!v) return NULL;
    AetherPythonVM* vm = (AetherPythonVM*)v;
    vm->instruction_budget = budget;
    vm->instruction_counter = budget;
    vm->instruction_tripped = 0;
    return NULL;
}

// --- Callback-side stack API ------------------------------------------------

int python_arg_count(void* v) {
    if (!v) return 0;
    AetherPythonVM* vm = (AetherPythonVM*)v;
    if (!vm->current_args) return 0;
    return (int)g_py.PyTuple_Size(vm->current_args);
}

static PyObject* get_arg_obj(AetherPythonVM* vm, int i) {
    if (!vm || !vm->current_args || i < 1) return NULL;
    Py_ssize_t idx = (Py_ssize_t)(i - 1);
    if (idx >= g_py.PyTuple_Size(vm->current_args)) return NULL;
    return g_py.PyTuple_GetItem(vm->current_args, idx); // Borrowed ref
}

int python_arg_type(void* v, int i) {
    if (!v) return AE_PY_TNIL;
    AetherPythonVM* vm = (AetherPythonVM*)v;
    PyObject* obj = get_arg_obj(vm, i);
    if (!obj || obj == g_py.py_none) return AE_PY_TNIL;

    // 1. Bool before int
    if (g_py.PyObject_IsInstance(obj, g_py.py_bool_type)) return AE_PY_TBOOL;

    // 2. Int
    if (g_py.PyObject_IsInstance(obj, g_py.py_long_type)) {
        int ov = 0;
        g_py.PyLong_AsLongLongAndOverflow(obj, &ov);
        if (ov != 0) return AE_PY_TOTHER;
        return AE_PY_TINT;
    }

    // 3. Float
    if (g_py.PyObject_IsInstance(obj, g_py.py_float_type)) return AE_PY_TFLOAT;

    // 4. Bytes
    if (g_py.PyObject_IsInstance(obj, g_py.py_bytes_type)) return AE_PY_TBYTES;

    // 5. String
    if (g_py.PyObject_IsInstance(obj, g_py.py_unicode_type)) return AE_PY_TSTR;

    // 6. Collections
    if (g_py.PyObject_IsInstance(obj, g_py.py_list_type) ||
        g_py.PyObject_IsInstance(obj, g_py.py_dict_type)) return AE_PY_TTABLE;

    return AE_PY_TOTHER;
}

const char* python_arg_str(void* v, int i) {
    if (!v) return "";
    AetherPythonVM* vm = (AetherPythonVM*)v;
    PyObject* obj = get_arg_obj(vm, i);
    if (!obj || !g_py.PyObject_IsInstance(obj, g_py.py_unicode_type)) return "";
    const char* s = g_py.PyUnicode_AsUTF8AndSize(obj, NULL);
    return s ? s : "";
}

int64_t python_arg_int(void* v, int i) {
    if (!v) return 0;
    AetherPythonVM* vm = (AetherPythonVM*)v;
    PyObject* obj = get_arg_obj(vm, i);
    if (!obj) return 0;
    if (g_py.PyObject_IsInstance(obj, g_py.py_long_type)) {
        int ov = 0;
        long long val = g_py.PyLong_AsLongLongAndOverflow(obj, &ov);
        if (ov != 0) return 0;
        return (int64_t)val;
    }
    return 0;
}

int python_arg_bool(void* v, int i) {
    if (!v) return 0;
    AetherPythonVM* vm = (AetherPythonVM*)v;
    PyObject* obj = get_arg_obj(vm, i);
    if (!obj) return 0;
    if (g_py.PyObject_IsInstance(obj, g_py.py_bool_type)) {
        int ov = 0;
        long long val = g_py.PyLong_AsLongLongAndOverflow(obj, &ov);
        return val != 0 ? 1 : 0;
    }
    return 0;
}

double python_arg_double(void* v, int i) {
    if (!v) return 0.0;
    AetherPythonVM* vm = (AetherPythonVM*)v;
    PyObject* obj = get_arg_obj(vm, i);
    if (!obj) return 0.0;
    if (g_py.PyObject_IsInstance(obj, g_py.py_float_type)) {
        return g_py.PyFloat_AsDouble(obj);
    }
    return 0.0;
}

const char* python_arg_bytes(void* v, int i, int* out_len) {
    if (out_len) *out_len = 0;
    if (!v) return "";
    AetherPythonVM* vm = (AetherPythonVM*)v;
    PyObject* obj = get_arg_obj(vm, i);
    if (!obj || !g_py.PyObject_IsInstance(obj, g_py.py_bytes_type)) return "";
    char* buf = NULL;
    Py_ssize_t len = 0;
    if (g_py.PyBytes_AsStringAndSize(obj, &buf, &len) == 0 && buf) {
        if (out_len) *out_len = (int)len;
        return buf;
    }
    return "";
}

// --- Callback pushers -------------------------------------------------------

void python_push_int(void* v, int64_t val) {
    if (!v) return;
    AetherPythonVM* vm = (AetherPythonVM*)v;
    if (vm->pushed_result) g_py.Py_DecRef(vm->pushed_result);
    vm->pushed_result = g_py.PyLong_FromLongLong(val);
}

void python_push_str(void* v, const char* val) {
    if (!v) return;
    AetherPythonVM* vm = (AetherPythonVM*)v;
    if (vm->pushed_result) g_py.Py_DecRef(vm->pushed_result);
    if (val) {
        vm->pushed_result = g_py.PyUnicode_FromString(val);
    } else {
        g_py.Py_IncRef(g_py.py_none);
        vm->pushed_result = g_py.py_none;
    }
}

void python_push_bool(void* v, int val) {
    if (!v) return;
    AetherPythonVM* vm = (AetherPythonVM*)v;
    if (vm->pushed_result) g_py.Py_DecRef(vm->pushed_result);
    vm->pushed_result = g_py.PyBool_FromLong(val != 0 ? 1 : 0);
}

void python_push_nil(void* v) {
    if (!v) return;
    AetherPythonVM* vm = (AetherPythonVM*)v;
    if (vm->pushed_result) g_py.Py_DecRef(vm->pushed_result);
    g_py.Py_IncRef(g_py.py_none);
    vm->pushed_result = g_py.py_none;
}

void python_push_double(void* v, double val) {
    if (!v) return;
    AetherPythonVM* vm = (AetherPythonVM*)v;
    if (vm->pushed_result) g_py.Py_DecRef(vm->pushed_result);
    vm->pushed_result = g_py.PyFloat_FromDouble(val);
}

void python_push_bytes(void* v, const char* val, int len) {
    if (!v) return;
    AetherPythonVM* vm = (AetherPythonVM*)v;
    if (vm->pushed_result) g_py.Py_DecRef(vm->pushed_result);
    if (val && len >= 0) {
        vm->pushed_result = g_py.PyBytes_FromStringAndSize(val, (Py_ssize_t)len);
    } else {
        g_py.Py_IncRef(g_py.py_none);
        vm->pushed_result = g_py.py_none;
    }
}

int python_raise_error(void* v, const char* msg) {
    if (!v) return 0;
    AetherPythonVM* vm = (AetherPythonVM*)v;
    vm->raise_error_flag = 1;
    if (vm->raise_error_msg) free(vm->raise_error_msg);
    vm->raise_error_msg = strdup(msg ? msg : "error");
    return 1;
}

#else
// Stubs when Python is not available
#include <stdio.h>
int python_init(void) {
    fprintf(stderr, "error: contrib.host.python not available (compile with AETHER_HAS_PYTHON)\n");
    return -1;
}
void python_finalize(void) {}
int python_run(const char* code) { (void)code; return python_init(); }
int python_run_sandboxed(void* perms,
    const char* code) {
  (void)perms; (void)code;
  return python_init();
}
int python_run_sandboxed_with_map(void* perms, const char* code,
    uint64_t token) {
  (void)perms; (void)code; (void)token;
  return python_init();
}

void* python_vm_new(void) { python_init(); return NULL; }
void python_vm_free(void* vm) { (void)vm; }
int python_vm_register(void* vm, const char* name, void* host_fn) { (void)vm; (void)name; (void)host_fn; return -1; }
void python_vm_set_global_str(void* vm, const char* name, const char* value) { (void)vm; (void)name; (void)value; }
void python_vm_set_global_int(void* vm, const char* name, int64_t value) { (void)vm; (void)name; (void)value; }
void python_vm_set_global_strlist(void* vm, const char* name, void* items) { (void)vm; (void)name; (void)items; }
int python_vm_eval(void* vm, const char* code) { (void)vm; (void)code; return -1; }
int python_vm_result_type(void* vm) { (void)vm; return 0; }
int64_t python_vm_result_int(void* vm) { (void)vm; return 0; }
const char* python_vm_result_str(void* vm) { (void)vm; return ""; }
int python_vm_result_bool(void* vm) { (void)vm; return 0; }
double python_vm_result_double(void* vm) { (void)vm; return 0.0; }
const char* python_vm_result_bytes(void* vm, int* out_len) { (void)vm; if (out_len) *out_len = 0; return ""; }
void python_vm_pop_result(void* vm) { (void)vm; }
const char* python_vm_set_instruction_limit(void* vm, int64_t budget, int every) { (void)vm; (void)budget; (void)every; return "execution limit unavailable"; }

int python_arg_count(void* vm) { (void)vm; return 0; }
int python_arg_type(void* vm, int i) { (void)vm; (void)i; return 0; }
const char* python_arg_str(void* vm, int i) { (void)vm; (void)i; return ""; }
int64_t python_arg_int(void* vm, int i) { (void)vm; (void)i; return 0; }
int python_arg_bool(void* vm, int i) { (void)vm; (void)i; return 0; }
double python_arg_double(void* vm, int i) { (void)vm; (void)i; return 0.0; }
const char* python_arg_bytes(void* vm, int i, int* out_len) { (void)vm; (void)i; if (out_len) *out_len = 0; return ""; }

void python_push_int(void* vm, int64_t v) { (void)vm; (void)v; }
void python_push_str(void* vm, const char* v) { (void)vm; (void)v; }
void python_push_bool(void* vm, int v) { (void)vm; (void)v; }
void python_push_nil(void* vm) { (void)vm; }
void python_push_double(void* vm, double v) { (void)vm; (void)v; }
void python_push_bytes(void* vm, const char* v, int len) { (void)vm; (void)v; (void)len; }
int python_raise_error(void* vm, const char* msg) { (void)vm; (void)msg; return -1; }
#endif
