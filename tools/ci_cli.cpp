#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "../vm.h"
#include "../CodeGen.h"
#include "../SymbolTable.h"
}

extern "C" void reportRuntimeErrorBridge(const char* msg)
{
    std::fprintf(stderr, "runtime error: %s\n", msg ? msg : "");
}

static void* ci_print_bool(ApiCallFrame* frame)
{
    std::printf("%s\n", GET_BOOL(0) ? "true" : "false");
    return nullptr;
}

static void* ci_print_int(ApiCallFrame* frame)
{
    std::printf("%d", GET_INT(0));
    return nullptr;
}

static void* ci_print_float(ApiCallFrame* frame)
{
    std::printf("%g", GET_FLOAT(0));
    return nullptr;
}

static void* ci_print_string(ApiCallFrame* frame)
{
    const char* text = GET_STRING(frame, 0);
    std::printf("%s", text ? text : "");
    return nullptr;
}

static void* ci_print_newline(ApiCallFrame* frame)
{
    (void)frame;
    std::printf("\n");
    return nullptr;
}

BEGIN_BIND_CLASS(IO)
(void)cls;
BIND_METHOD(printb, ci_print_bool)
BIND_METHOD(printI, ci_print_int)
BIND_METHOD(printF, ci_print_float)
BIND_METHOD(printS, ci_print_string)
BIND_METHOD(nl, ci_print_newline)
END_BIND_CLASS()

static void usage()
{
    std::fprintf(stderr,
        "usage: ci [options] <script.ci>\n"
        "\n"
        "options:\n"
        "  --no-setup       compile and run globals only\n"
        "  --update <dt>    call update(dt) once after setup, if present\n"
        "  --debug          start the VM in stepping/debug mode for globals\n"
        "  -h, --help       show this help\n");
}

int main(int argc, char** argv)
{
    const char* script = nullptr;
    bool callSetup = true;
    bool debugGlobals = false;
    bool callUpdate = false;
    float updateDt = 0.016f;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            usage();
            return EXIT_SUCCESS;
        }
        if (std::strcmp(argv[i], "--no-setup") == 0) {
            callSetup = false;
            continue;
        }
        if (std::strcmp(argv[i], "--debug") == 0) {
            debugGlobals = true;
            continue;
        }
        if (std::strcmp(argv[i], "--update") == 0) {
            if (i + 1 >= argc) {
                usage();
                return EXIT_FAILURE;
            }
            callUpdate = true;
            updateDt = (float)std::atof(argv[++i]);
            continue;
        }
        if (argv[i][0] == '-') {
            std::fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage();
            return EXIT_FAILURE;
        }
        if (script) {
            std::fprintf(stderr, "only one script file can be passed\n");
            usage();
            return EXIT_FAILURE;
        }
        script = argv[i];
    }

    if (!script) {
        usage();
        return EXIT_FAILURE;
    }

    State* state = createState();
    if (!state) {
        std::fprintf(stderr, "failed to create CI state\n");
        return EXIT_FAILURE;
    }

    bind_IO(state);

    int exitCode = EXIT_SUCCESS;
    if (!compile(state, script)) {
        std::fprintf(stderr, "compile failed: %s\n", script);
        exitCode = EXIT_FAILURE;
        goto cleanup;
    }

    vm_init(state);
    vm_run_global(state, debugGlobals);

    if (state->vm->had_runtime_error) {
        exitCode = EXIT_FAILURE;
        goto cleanup;
    }

    if (callSetup && state->vm->setup_ip) {
        if (!vm_call_direct(state, state->vm->setup_ip, 0.0f))
            exitCode = EXIT_FAILURE;
    }

    if (exitCode == EXIT_SUCCESS && callUpdate && state->vm->update_ip) {
        if (!vm_call_direct(state, state->vm->update_ip, updateDt))
            exitCode = EXIT_FAILURE;
    }

cleanup:
    if (state->vm) {
        vm_free(state);
        state->vm = nullptr;
    }
    freeState(state);
    return exitCode;
}
