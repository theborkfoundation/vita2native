/*
 * v2n-extract - turn a PlayStation Vita .pkg into the layout vita2native loads.
 *
 * Three layers, in order:
 *   1. pkg container  (src/pkg_reader.c, public AES keys, no license needed)
 *   2. PFS            (psvpfstools, needs the work.bin/zRIF license)
 *   3. SELF           (Vita3K's decrypt_fself, same license, keys are built in)
 *
 * The result is <out>/<TITLE_ID>/ with a decrypted eboot.bin, which is exactly
 * what tools/v2n title <ID> expects on the device.
 */

#include "pkg_reader.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <PsvPfsParserConfig.h>
#include <packages/sce_types.h>
#include <rif2zrif.h>

namespace fs = std::filesystem;

static const char *kSelfMagic = "SCE\0";

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

static bool read_license(const std::string &path, std::string &zrif_out, std::string &error)
{
    /* rif2zrif() takes the stream size from tellg(), so it has to be opened at
       the end - same as Vita3K does. */
    std::ifstream file(path, std::ios::binary | std::ios::ate);

    if (!file) {
        error = "cannot open " + path;
        return false;
    }
    zrif_out = rif2zrif(file);
    if (zrif_out.empty()) {
        error = path + " is not a readable work.bin license";
        return false;
    }
    return true;
}

/* Every SELF in the tree becomes the decrypted form vita2native loads. */
static void decrypt_self_tree(const fs::path &root, const uint8_t klic[16], unsigned int &count,
                              unsigned int &failed)
{
    std::error_code ec;

    for (const auto &item : fs::recursive_directory_iterator(root, ec)) {
        std::vector<uint8_t> input;
        std::ifstream file(item.path(), std::ios::binary);
        std::vector<uint8_t> output;

        if (!item.is_regular_file() || !file)
            continue;
        input.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        if (input.size() < 4 || memcmp(input.data(), kSelfMagic, 4) != 0)
            continue;

        output = decrypt_fself(input, klic);
        if (output.empty()) {
            fprintf(stderr, "v2n-extract: could not decrypt %s\n",
                    item.path().lexically_relative(root).string().c_str());
            failed++;
            continue;
        }
        std::ofstream out(item.path(), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(output.data()), (std::streamsize)output.size());
        count++;
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

    char error[512] = {0};
    v2n_pkg *pkg = v2n_pkg_open(pkg_path, error, sizeof(error));

    if (!pkg) {
        fprintf(stderr, "v2n-extract: %s: %s\n", pkg_path, error);
        return 1;
    }
    if (list_only) {
        list_package(pkg);
        v2n_pkg_close(pkg);
        return 0;
    }

    /* Resolve the license first: without it the extracted files stay
       PFS-encrypted, so there is no point in writing 200+ MB to disk. */
    std::string zrif;
    std::string license_error;

    if (zrif_arg) {
        zrif = zrif_arg;
    } else if (work_bin) {
        if (!read_license(work_bin, zrif, license_error)) {
            fprintf(stderr, "v2n-extract: %s\n", license_error.c_str());
            v2n_pkg_close(pkg);
            return 1;
        }
    } else {
        fprintf(stderr, "v2n-extract: no license; pass --work-bin <work.bin> or --zrif <string>\n");
        fprintf(stderr, "             (--list works without one)\n");
        v2n_pkg_close(pkg);
        return 2;
    }

    const std::string title_id = v2n_pkg_title_id(pkg);
    if (title_id.empty()) {
        fprintf(stderr, "v2n-extract: no title id in %s\n", v2n_pkg_content_id(pkg));
        v2n_pkg_close(pkg);
        return 1;
    }

    /* Layer 1: the container. */
    const fs::path root(out_dir);
    const fs::path temp = root / (".v2n-temp-" + std::to_string((long)getpid()));
    std::error_code ec;

    fs::create_directories(root, ec);
    fs::remove_all(temp, ec);

    printf("v2n-extract: %s (%s)\n", v2n_pkg_title(pkg)[0] ? v2n_pkg_title(pkg) : title_id.c_str(),
           title_id.c_str());
    printf("v2n-extract: decrypting the pkg container\n");
    if (v2n_pkg_extract(pkg, temp.string().c_str(), error, sizeof(error)) != 0) {
        fprintf(stderr, "v2n-extract: %s\n", error);
        fs::remove_all(temp, ec);
        v2n_pkg_close(pkg);
        return 1;
    }
    v2n_pkg_close(pkg);

    /* Layer 2: PFS, which needs the license. */
    const fs::path destination = root / title_id;

    printf("v2n-extract: decrypting the PFS (this is the slow part)\n");
    {
        std::string source = temp.string();
        std::string target = destination.string();
        std::string f00d_arg;

        fs::remove_all(destination, ec);
        if (execute(zrif, source, target, F00DEncryptorTypes::native, f00d_arg) < 0) {
            fprintf(stderr, "v2n-extract: PFS decryption failed (wrong license for this pkg?)\n");
            if (!keep_temp)
                fs::remove_all(temp, ec);
            return 1;
        }
    }
    if (!keep_temp)
        fs::remove_all(temp, ec);

    /* Layer 3: SELF -> decrypted SELF. */
    if (!skip_self) {
        std::vector<uint8_t> klic = get_temp_klicensee(zrif);
        unsigned int count = 0, failed = 0;

        if (klic.size() < 16) {
            fprintf(stderr, "v2n-extract: could not derive a klicensee from the license\n");
            return 1;
        }
        printf("v2n-extract: decrypting SELF files\n");
        decrypt_self_tree(destination, klic.data(), count, failed);
        printf("v2n-extract: %u SELF file(s) decrypted", count);
        if (failed)
            printf(", %u failed", failed);
        printf("\n");
        if (failed)
            return 1;
    }

    printf("v2n-extract: done: %s\n", destination.string().c_str());
    return 0;
}
