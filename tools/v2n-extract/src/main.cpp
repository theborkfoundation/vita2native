#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "extract.h"
#include "pkg_reader.h"

static void usage()
{
    printf("v2n-extract - decrypt a Vita .pkg into a runnable game directory\n\n"
           "usage: v2n-extract --pkg <file.pkg> [options]\n\n"
           "  --pkg <file>        package to read\n"
           "  --out <dir>         output directory (default: .)\n"
           "  --work-bin <file>   NoNpDrm work.bin license (replaces --zrif)\n"
           "  --zrif <string>     license as a zRIF string (replaces --work-bin)\n"
           "  --list              print the contents and exit (no license needed)\n"
           "  --keep-temp         keep the intermediate pkg extraction\n"
           "  --no-self           stop before SELF decryption\n"
           "  -h, --help          this text\n\n"
           "Without a license the pkg layer is still decrypted, but the game\n"
           "files stay PFS-encrypted; the tool then stops with an error.\n");
}

static const char *content_type_name(uint32_t type)
{
    switch (type) {
    case 0x15:
        return "app";
    case 0x16:
        return "dlc";
    case 0x1A:
        return "patch";
    case 0x1F:
        return "theme";
    default:
        return "unknown";
    }
}

static void list_package(const v2n_pkg *pkg)
{
    printf("content id: %s\n", v2n_pkg_content_id(pkg));
    printf("title id:   %s\n", v2n_pkg_title_id(pkg));
    printf("title:      %s\n", v2n_pkg_title(pkg));
    printf("type:       0x%02x (%s)\n", v2n_pkg_content_type(pkg),
           content_type_name(v2n_pkg_content_type(pkg)));
    printf("entries:    %zu\n\n", v2n_pkg_entry_count(pkg));

    for (size_t i = 0; i < v2n_pkg_entry_count(pkg); i++) {
        const v2n_pkg_entry *entry = v2n_pkg_entry_at(pkg, i);

        printf("  %-64s %10llu %s\n", entry->name, (unsigned long long)entry->size,
               entry->is_dir ? "dir" : "");
    }
}

int main(int argc, char **argv)
{
    const char *pkg_path = nullptr;
    const char *out_dir = ".";
    const char *work_bin = nullptr;
    const char *zrif_arg = nullptr;
    bool list_only = false;
    bool keep_temp = false;
    bool skip_self = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        auto next = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                fprintf(stderr, "v2n-extract: %s needs a value\n", name);
                exit(2);
            }
            return argv[++i];
        };

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage();
            return 0;
        } else if (strcmp(arg, "--pkg") == 0) {
            pkg_path = next("--pkg");
        } else if (strcmp(arg, "--out") == 0) {
            out_dir = next("--out");
        } else if (strcmp(arg, "--work-bin") == 0) {
            work_bin = next("--work-bin");
        } else if (strcmp(arg, "--zrif") == 0) {
            zrif_arg = next("--zrif");
        } else if (strcmp(arg, "--list") == 0) {
            list_only = true;
        } else if (strcmp(arg, "--keep-temp") == 0) {
            keep_temp = true;
        } else if (strcmp(arg, "--no-self") == 0) {
            skip_self = true;
        } else {
            fprintf(stderr, "v2n-extract: unknown option %s\n", arg);
            usage();
            return 2;
        }
    }

    if (!pkg_path) {
        usage();
        return 2;
    }

    if (list_only) {
        char error[512] = { 0 };
        v2n_pkg *pkg = v2n_pkg_open(pkg_path, error, sizeof(error));
        if (!pkg) {
            fprintf(stderr, "v2n-extract: %s: %s\n", pkg_path, error);
            return 1;
        }
        list_package(pkg);
        v2n_pkg_close(pkg);
        return 0;
    }
    V2nExtractOptions options;
    options.pkg_path = pkg_path;
    options.out_dir = out_dir;
    options.work_bin = work_bin;
    options.zrif = zrif_arg;
    options.keep_temp = keep_temp;
    options.skip_self = skip_self;
    return v2n_extract_package(options);
}
