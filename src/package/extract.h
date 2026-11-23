#pragma once

#include <cstdio>

// One extraction at a time; call from a background thread to keep the UI responsive.
// Caller supplies an isolated output directory: existing titles there are replaced.
// The progress callback runs on the calling thread.
struct V2nExtractOptions {
    const char *pkg_path = nullptr;
    const char *out_dir = ".";
    const char *work_bin = nullptr;
    const char *zrif = nullptr;
    std::FILE *log = nullptr; // Pipeline diagnostics; third-party engines may log to the console.
    void (*progress)(void *context, const char *message) = nullptr;
    void *progress_context = nullptr;
    bool keep_temp = false;
    bool skip_self = false;
};

int v2n_extract_package(const V2nExtractOptions &options);
