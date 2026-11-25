/*
 * Shared PlayStation Vita package, PFS and SELF extraction pipeline.
 *
 * Three layers, in order:
 *   1. pkg container  (pkg_reader.c, public AES keys, no license needed)
 *   2. PFS            (psvpfstools, needs the work.bin/zRIF license)
 *   3. SELF           (Vita3K's decrypt_fself, same license, keys are built in)
 *
 * The result is <out>/<TITLE_ID>/ with a decrypted eboot.bin.
 */

#include <PsvPfsParserConfig.h>
#include <array>
#include <b64/cencode.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <packages/sce_types.h>
#include <rif2zrif.h>
#include <zRIF/keyflate.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "extract.h"
#include "pkg_reader.h"

namespace fs = std::filesystem;

static const char *kSelfMagic = "SCE\0";

static bool read_license(const std::string &path, std::string &zrif_out, std::string &error)
{
    // Encode the fixed-size license without the upstream helper's key logging
    // or its unchecked read into a fixed-size buffer.
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    std::array<unsigned char, 512> license{};
    if (!file || file.tellg() != static_cast<std::streamoff>(license.size())) {
        error = path + " is not a 512-byte work.bin license";
        return false;
    }
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(license.data()), license.size())) {
        error = "cannot read " + path;
        return false;
    }
    std::array<unsigned char, 2048> compressed{};
    int length = deflateKey(license.data(), license.size(), compressed.data(), compressed.size());
    if (length < 0 || length > static_cast<int>(compressed.size()) - 2) {
        error = "cannot encode " + path;
        return false;
    }
    if (length % 3)
        length += 3 - length % 3;
    std::array<char, 4096> encoded{};
    base64_encodestate state;
    base64_init_encodestate(&state);
    int encoded_length = base64_encode_block(reinterpret_cast<char *>(compressed.data()), length,
                                             encoded.data(), &state);
    encoded_length += base64_encode_blockend(encoded.data() + encoded_length, &state);
    zrif_out.assign(encoded.data(), encoded_length);
    return true;
}

/* Every SELF in the tree becomes the decrypted form vita2native loads. */
static void decrypt_self_tree(const fs::path &root, const uint8_t klic[16], unsigned int &count,
                              unsigned int &failed, std::FILE *err)
{
    std::error_code ec;

    for (const auto &item : fs::recursive_directory_iterator(root, ec)) {
        if (!item.is_regular_file(ec))
            continue;

        std::ifstream file(item.path(), std::ios::binary | std::ios::ate);
        if (!file)
            continue;
        const std::streamoff size = file.tellg();
        if (size < 4)
            continue;

        char magic[4];
        file.seekg(0);
        if (!file.read(magic, sizeof(magic)) || memcmp(magic, kSelfMagic, sizeof(magic)) != 0)
            continue;

        std::vector<uint8_t> input(static_cast<size_t>(size));
        std::memcpy(input.data(), magic, sizeof(magic));
        if (size > 4 &&
            !file.read(reinterpret_cast<char *>(input.data() + 4), size - 4))
            continue;

        std::vector<uint8_t> output = decrypt_fself(input, klic);
        if (output.empty()) {
            fprintf(err, "package: could not decrypt %s\n",
                    item.path().lexically_relative(root).string().c_str());
            failed++;
            continue;
        }
        file.close();
        std::ofstream out(item.path(), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(output.data()), (std::streamsize)output.size());
        count++;
    }
}

int v2n_extract_package(const V2nExtractOptions &options)
{
    std::FILE *out = options.log ? options.log : stdout;
    std::FILE *err = options.log ? options.log : stderr;
    auto progress = [&](const char *message) {
        if (options.progress)
            options.progress(options.progress_context, message);
    };
    const char *pkg_path = options.pkg_path;
    const char *out_dir = options.out_dir;
    const char *work_bin = options.work_bin;
    const char *zrif_arg = options.zrif;
    bool keep_temp = options.keep_temp;
    bool skip_self = options.skip_self;
    if (!pkg_path || !out_dir) {
        fprintf(err, "package extraction: missing package or output path\n");
        return 2;
    }
    char error[512] = { 0 };
    v2n_pkg *pkg = v2n_pkg_open(pkg_path, error, sizeof(error));

    if (!pkg) {
        fprintf(err, "package: %s: %s\n", pkg_path, error);
        return 1;
    }

    /* Resolve the license first: without it the extracted files stay
       PFS-encrypted, so there is no point in writing 200+ MB to disk. */
    std::string zrif;
    std::string license_error;

    if (zrif_arg) {
        zrif = zrif_arg;
    } else if (work_bin) {
        if (!read_license(work_bin, zrif, license_error)) {
            fprintf(err, "package: %s\n", license_error.c_str());
            v2n_pkg_close(pkg);
            return 1;
        }
    } else {
        fprintf(err, "package: no license; pass --work-bin <work.bin> or --zrif <string>\n");
        v2n_pkg_close(pkg);
        return 2;
    }

    const std::string title_id = v2n_pkg_title_id(pkg);
    if (title_id.empty()) {
        fprintf(err, "package: no title id in %s\n", v2n_pkg_content_id(pkg));
        v2n_pkg_close(pkg);
        return 1;
    }

    /* Layer 1: the container. */
    const fs::path root(out_dir);
    const fs::path temp = root / (".v2n-temp-" + std::to_string((long)getpid()));
    std::error_code ec;

    fs::create_directories(root, ec);
    fs::remove_all(temp, ec);

    fprintf(out, "package: %s (%s)\n", v2n_pkg_title(pkg)[0] ? v2n_pkg_title(pkg) : title_id.c_str(),
           title_id.c_str());
    progress("Decrypting package...");
    fprintf(out, "package: decrypting the pkg container\n");
    if (v2n_pkg_extract(pkg, temp.string().c_str(), error, sizeof(error)) != 0) {
        fprintf(err, "package: %s\n", error);
        fs::remove_all(temp, ec);
        v2n_pkg_close(pkg);
        return 1;
    }
    v2n_pkg_close(pkg);

    /* Layer 2: PFS, which needs the license. */
    const fs::path destination = root / title_id;

    progress("Decrypting game files...");
    fprintf(out, "package: decrypting the PFS (this is the slow part)\n");
    {
        std::string source = temp.string();
        std::string target = destination.string();
        std::string f00d_arg;

        fs::remove_all(destination, ec);
        if (execute(zrif, source, target, F00DEncryptorTypes::native, f00d_arg) < 0) {
            fprintf(err, "package: PFS decryption failed (wrong license for this pkg?)\n");
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
            fprintf(err, "package: could not derive a klicensee from the license\n");
            return 1;
        }
        progress("Decrypting executables...");
        fprintf(out, "package: decrypting SELF files\n");
        decrypt_self_tree(destination, klic.data(), count, failed, err);
        fprintf(out, "package: %u SELF file(s) decrypted", count);
        if (failed)
            fprintf(out, ", %u failed", failed);
        fprintf(out, "\n");
        if (failed)
            return 1;
    }

    fprintf(out, "package: done: %s\n", destination.string().c_str());
    return 0;
}
