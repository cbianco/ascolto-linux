#define _DARWIN_C_SOURCE
#include "prepare_bf16.h"
#include "json.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif

namespace {
using Json = nlohmann::json;
constexpr size_t chunk = 1024 * 1024, max_matrix = 32 * 1024 * 1024;
void require(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
struct Fd {
    int value;
    explicit Fd(int v) : value(v) { require(v >= 0, "Cannot open a required preparation file"); }
    ~Fd() { close(value); }
    Fd(const Fd &) = delete;
};
struct Cancel {
    ascolto_prepare_cancel callback;
    void *userdata;
    void check() const { if (callback && callback(userdata)) throw std::runtime_error("Model preparation cancelled"); }
};
struct Digest {
#ifdef __APPLE__
    CC_SHA256_CTX state{};
    Digest() { require(CC_SHA256_Init(&state), "Cannot initialize SHA-256"); }
    void update(const void *data, size_t bytes) { require(CC_SHA256_Update(&state, data, (CC_LONG)bytes), "SHA-256 failed"); }
    void finish(unsigned char *bytes) { require(CC_SHA256_Final(bytes, &state), "SHA-256 failed"); }
#else
    EVP_MD_CTX *state = EVP_MD_CTX_new();
    Digest() { if (!state || !EVP_DigestInit_ex(state, EVP_sha256(), nullptr)) { EVP_MD_CTX_free(state); state = nullptr; throw std::runtime_error("Cannot initialize SHA-256"); } }
    ~Digest() { EVP_MD_CTX_free(state); }
    void update(const void *data, size_t bytes) { require(EVP_DigestUpdate(state, data, bytes), "SHA-256 failed"); }
    void finish(unsigned char *bytes) { require(EVP_DigestFinal_ex(state, bytes, nullptr), "SHA-256 failed"); }
#endif
    std::string finish() {
        unsigned char bytes[32]; finish(bytes);
        char text[65]; for (int i = 0; i < 32; ++i) std::snprintf(text + 2 * i, 3, "%02x", bytes[i]);
        return text;
    }
};
uint64_t product(uint64_t a, uint64_t b) {
    require(!b || a <= UINT64_MAX / b, "Preparation dimension overflow"); return a * b;
}
void regular(int fd, uint64_t bytes) {
    struct stat info{};
    require(!fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_size >= 0 && uint64_t(info.st_size) == bytes,
            "Preparation file type or size mismatch");
}
void read_at(int fd, void *data, size_t bytes, uint64_t offset, const Cancel &cancel) {
    require(offset <= INT64_MAX && bytes <= uint64_t(INT64_MAX) - offset, "Preparation read offset overflow");
    size_t done = 0;
    while (done < bytes) {
        cancel.check();
        ssize_t count = pread(fd, static_cast<char *>(data) + done, std::min(chunk, bytes - done), off_t(offset + done));
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "Preparation input read failed"); done += size_t(count);
    }
}
std::string hash(int fd, uint64_t bytes, const Cancel &cancel) {
    Digest digest; std::vector<unsigned char> data(chunk);
    for (uint64_t offset = 0; offset < bytes;) {
        size_t n = size_t(std::min<uint64_t>(data.size(), bytes - offset));
        read_at(fd, data.data(), n, offset, cancel); digest.update(data.data(), n); offset += n;
    }
    cancel.check(); return digest.finish();
}
std::vector<unsigned char> resource(int directory, const std::string &name, const std::string &sha,
                                    size_t limit, const Cancel &cancel) {
    require(name == "recipe.json" || name == "decoder.header" || name == "encoder.header" || name == "constants.bin",
            "Invalid preparation resource name");
    Fd fd(openat(directory, name.c_str(), O_RDONLY | O_NOFOLLOW));
    struct stat info{};
    require(!fstat(fd.value, &info) && S_ISREG(info.st_mode) && info.st_size >= 0 && uint64_t(info.st_size) <= limit,
            "Invalid preparation resource size");
    std::vector<unsigned char> bytes(size_t(info.st_size));
    read_at(fd.value, bytes.data(), bytes.size(), 0, cancel);
    Digest digest; digest.update(bytes.data(), bytes.size()); require(digest.finish() == sha, "Preparation resource checksum mismatch");
    return bytes;
}
struct Writer {
    int fd;
    Digest digest;
    const Cancel &cancel;
    uint64_t position = 0;
    void write(const void *data, size_t bytes) {
        require(bytes <= UINT64_MAX - position, "Preparation output overflow");
        size_t done = 0;
        while (done < bytes) {
            cancel.check();
            size_t want = std::min(chunk, bytes - done);
            ssize_t n = ::write(fd, static_cast<const char *>(data) + done, want);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "Cannot write prepared model; check free disk space");
            digest.update(static_cast<const char *>(data) + done, size_t(n));
            done += size_t(n); position += uint64_t(n);
        }
    }
    void pad(uint64_t offset) {
        require(offset >= position && offset - position <= 31, "Invalid tensor offset or padding");
        unsigned char zero[32]{}; write(zero, size_t(offset - position));
    }
};
void tensor(int source, uint64_t source_bytes, const Json &entry, const std::vector<unsigned char> &constants,
            Writer &writer) {
    const uint64_t output_bytes = entry.at("bytes").get<uint64_t>();
    require(output_bytes > 0, "Empty preparation tensor");
    auto kind = entry.at("kind").get<std::string>();
    if (kind == "constant") {
        uint64_t offset = entry.at("constant_offset").get<uint64_t>();
        require(offset <= constants.size() && output_bytes <= constants.size() - offset, "Invalid constant extent");
        writer.write(constants.data() + size_t(offset), size_t(output_bytes)); return;
    }
    uint64_t offset = entry.at("source_offset").get<uint64_t>(), bytes = entry.at("source_bytes").get<uint64_t>();
    require(offset <= source_bytes && bytes <= source_bytes - offset && bytes > 0, "Invalid source tensor extent");
    if (kind == "permute_bf16") {
        auto shape = entry.at("source_shape").get<std::vector<uint64_t>>();
        uint64_t heads = entry.at("heads").get<uint64_t>();
        require(shape.size() == 2 && heads && shape[0] && shape[1] && shape[0] % heads == 0 && (shape[0] / heads) % 2 == 0,
                "Invalid RoPE permutation dimensions");
        uint64_t matrix = product(product(shape[0], shape[1]), 2);
        require(matrix <= max_matrix && matrix == bytes && bytes == output_bytes, "Invalid RoPE permutation byte size");
        std::vector<uint16_t> input(size_t(matrix / 2), 0), output(size_t(matrix / 2), 0);
        read_at(source, input.data(), size_t(matrix), offset, writer.cancel);
        uint64_t half = shape[0] / heads / 2;
        for (uint64_t head = 0; head < heads; ++head) for (uint64_t row = 0; row < half; ++row) {
            writer.cancel.check();
            for (uint64_t parity = 0; parity < 2; ++parity)
                std::memcpy(output.data() + (head * half * 2 + row * 2 + parity) * shape[1],
                            input.data() + (head * half * 2 + parity * half + row) * shape[1], size_t(shape[1] * 2));
        }
        writer.write(output.data(), size_t(matrix)); return;
    }
    if (kind == "transpose_bf16") {
        auto shape = entry.at("source_shape").get<std::vector<uint64_t>>();
        require(shape.size() == 3 && shape[0] && shape[1] && shape[2], "Invalid expert transpose dimensions");
        uint64_t elements = product(shape[1], shape[2]), matrix = product(elements, 2);
        require(matrix <= max_matrix && product(matrix, shape[0]) == bytes && bytes == output_bytes,
                "Invalid expert transpose byte size");
        std::vector<uint16_t> input(size_t(elements), 0), output(size_t(elements), 0);
        for (uint64_t expert = 0; expert < shape[0]; ++expert) {
            read_at(source, input.data(), size_t(matrix), offset + expert * matrix, writer.cancel);
            for (uint64_t row = 0; row < shape[1]; ++row) {
                if (!(row % 128)) writer.cancel.check();
                for (uint64_t col = 0; col < shape[2]; ++col) output[size_t(col * shape[1] + row)] = input[size_t(row * shape[2] + col)];
            }
            writer.write(output.data(), size_t(matrix));
        }
        return;
    }
    bool expand = kind == "bf16_to_f32";
    require(expand || kind == "copy", "Unsupported preparation operation");
    require((!expand && bytes == output_bytes) || (expand && bytes % 2 == 0 && product(bytes, 2) == output_bytes),
            "Invalid conversion extent size");
    std::vector<unsigned char> input(chunk);
    std::vector<uint32_t> expanded(expand ? chunk / 2 : 0);
    for (uint64_t done = 0; done < bytes;) {
        size_t count = size_t(std::min<uint64_t>(input.size(), bytes - done));
        read_at(source, input.data(), count, offset + done, writer.cancel);
        if (expand) {
            for (size_t i = 0; i < count / 2; ++i) {
                uint16_t bits; std::memcpy(&bits, input.data() + 2 * i, 2);
                expanded[i] = uint32_t(bits) << 16;
            }
            writer.write(expanded.data(), count * 2);
        } else writer.write(input.data(), count);
        done += count;
    }
}
}

int ascolto_prepare_bf16(int staging_fd, int verified_input_fd, const char *resource_path, const char *recipe_sha,
                        ascolto_prepare_cancel callback, void *userdata, char *error, size_t capacity) noexcept {
    std::array<bool, 2> created{}, published{};
    int cleanup_directory = -1;
    try {
        Cancel cancel{callback, userdata}; cancel.check();
        uint16_t endian = 1; require(*reinterpret_cast<unsigned char *>(&endian) == 1, "Preparation requires a little-endian host");
        Fd directory(dup(staging_fd));
        struct stat stage_info{};
        require(!fstat(directory.value, &stage_info) && S_ISDIR(stage_info.st_mode), "Invalid preparation staging directory");
        cleanup_directory = dup(directory.value); require(cleanup_directory >= 0, "Cannot retain preparation directory");
        Fd resources(open(resource_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
        auto serialized = resource(resources.value, "recipe.json", recipe_sha, 8 * 1024 * 1024, cancel);
        auto recipe = Json::parse(serialized.begin(), serialized.end());
        require(recipe.at("version") == 1, "Unsupported preparation recipe version");
        auto source_size = recipe.at("source_bytes").get<uint64_t>();
        Fd source(verified_input_fd >= 0 ? dup(verified_input_fd) : openat(directory.value, "model.safetensors", O_RDONLY | O_NOFOLLOW)); regular(source.value, source_size);
#ifdef __APPLE__
        require(!fcntl(source.value, F_NOCACHE, 1), "Cannot disable source file caching");
#endif
        if (verified_input_fd < 0)
            require(hash(source.value, source_size, cancel) == recipe.at("source_sha256").get<std::string>(), "Reference checkpoint checksum mismatch");
        auto constants = resource(resources.value, "constants.bin", recipe.at("constants_sha256"), 16 * 1024 * 1024, cancel);
        auto outputs = recipe.at("outputs"); require(outputs.is_array() && outputs.size() == 2, "Invalid preparation output list");
        for (size_t i = 0; i < outputs.size(); ++i) {
            const auto &output = outputs[i];
            std::string name = output.at("name");
            require(name == (i == 0 ? "decoder.gguf" : "encoder.gguf"), "Invalid prepared model name");
            uint64_t expected_bytes = output.at("bytes").get<uint64_t>();
            auto header = resource(resources.value, i == 0 ? "decoder.header" : "encoder.header",
                                   output.at("header_sha256"), 32 * 1024 * 1024, cancel);
            require(header.size() <= expected_bytes && expected_bytes <= INT64_MAX, "Invalid prepared model size");
            std::string temporary = name + ".writing";
            Fd fd(openat(directory.value, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
            created[i] = true;
#ifdef __APPLE__
            require(!fcntl(fd.value, F_NOCACHE, 1), "Cannot disable output file caching");
#endif
            Writer writer{fd.value, {}, cancel}; writer.write(header.data(), header.size());
            const auto &tensors = output.at("tensors");
            require(tensors.is_array() && tensors.size() <= 10000, "Invalid preparation tensor list");
            for (const auto &entry : tensors) {
                uint64_t offset = entry.at("offset").get<uint64_t>(), bytes = entry.at("bytes").get<uint64_t>();
                require(offset <= expected_bytes && bytes <= expected_bytes - offset, "Prepared tensor exceeds file bounds");
                writer.pad(offset); tensor(source.value, source_size, entry, constants, writer);
            }
            writer.pad(expected_bytes);
            require(writer.position == expected_bytes && writer.digest.finish() == output.at("sha256").get<std::string>(), "Prepared model checksum mismatch");
            regular(fd.value, expected_bytes); require(!fsync(fd.value), "Cannot synchronize prepared model"); cancel.check();
            // Exclusive publication inside the private staging directory, without replacing a file.
            require(!linkat(directory.value, temporary.c_str(), directory.value, name.c_str(), 0), "Cannot publish prepared model");
            published[i] = true;
            require(!unlinkat(directory.value, temporary.c_str(), 0), "Cannot finalize prepared model");
            created[i] = false;
        }
        require(!fsync(directory.value), "Cannot synchronize preparation directory"); cancel.check();
        close(cleanup_directory); cleanup_directory = -1;
        return 0;
    } catch (const std::exception &exception) {
        if (error && capacity) std::snprintf(error, capacity, "%.230s", exception.what());
    } catch (...) {
        if (error && capacity) std::snprintf(error, capacity, "Native model preparation failed");
    }
    if (cleanup_directory >= 0) {
        const char *names[] = {"decoder.gguf", "encoder.gguf"};
        const char *temporary[] = {"decoder.gguf.writing", "encoder.gguf.writing"};
        for (size_t i = 0; i < 2; ++i) {
            if (created[i]) unlinkat(cleanup_directory, temporary[i], 0);
            if (published[i]) unlinkat(cleanup_directory, names[i], 0);
        }
        close(cleanup_directory);
    }
    return -1;
}
