// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// bktr_romfs_check: reads the merged (base + update BKTR patch) romfs of a game through Eden's own
// NCA/VFS stack -- the same NCA(update_raw, base_nca) path PatchManager::PatchRomFS takes at boot --
// and compares every file with a reference dump made by an independent reader.
//
//   bktr_romfs_check <base.nsp> <update.nsp> <reference romfs dir> [options]
//     --threads N      read files from N threads at once (default 1)
//     --chunk SIZE     read each file in SIZE-byte pieces (default: whole file in one read)
//     --odd            read each file in pseudo-random odd-sized pieces (game-like access)
//     --only SUBSTR    only files whose path contains SUBSTR
//     --list           print the romfs-relative data offset of every checked file
//     --raw N          instead of per-file reads, do N random reads of the raw romfs storage
//                      (random unaligned offset, 1 B..16 MiB, spanning many files) and check
//                      every file byte they cover
//
// Keys come from Eden's normal keys dir (point XDG_DATA_HOME at an isolated profile).
// Exit code 0 = every file matched.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/logging.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/loader.h"

namespace {

struct Item {
    std::string path;
    FileSys::VirtualFile file;
    u64 offset;
};

void Collect(const FileSys::VirtualDir& dir, const std::string& prefix, std::vector<Item>& out) {
    for (const auto& f : dir->GetFiles()) {
        // ExtractRomFS only ever creates OffsetVfsFiles over the romfs storage.
        const u64 off = std::static_pointer_cast<FileSys::OffsetVfsFile>(f)->GetOffset();
        out.push_back({prefix + f->GetName(), f, off});
    }
    for (const auto& d : dir->GetSubdirectories()) {
        Collect(d, prefix + d->GetName() + "/", out);
    }
}

std::shared_ptr<FileSys::NCA> FindProgram(const FileSys::NSP& nsp, const FileSys::NCA* base,
                                          bool want_update) {
    for (const auto& f : nsp.GetFiles()) {
        const auto& n = f->GetName();
        if (n.size() < 4 || n.substr(n.size() - 4) != ".nca" ||
            n.find(".cnmt.") != std::string::npos) {
            continue;
        }
        auto nca = std::make_shared<FileSys::NCA>(f, base);
        if (nca->GetType() != FileSys::NCAContentType::Program) {
            continue;
        }
        if (want_update != nca->IsUpdate()) {
            continue;
        }
        return nca;
    }
    return nullptr;
}

// Random raw-storage reads spanning file boundaries, like a game streaming its romfs through
// IStorage::Read. Every file byte a read covers is checked against the reference file.
int RawCheck(const FileSys::VirtualFile& romfs, std::vector<Item> items, const std::string& ref_dir,
             u64 count, int threads) {
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.offset < b.offset; });
    std::erase_if(items, [](const Item& it) { return it.file->GetSize() == 0; });
    const u64 lo = items.front().offset;
    const u64 hi = items.back().offset + items.back().file->GetSize();
    std::atomic<u64> next{0}, bad_reads{0}, checked_bytes{0};
    std::mutex out_mutex;
    auto worker = [&](int tid) {
        u64 rng = 0xD1B54A32D192ED03ULL * static_cast<u64>(tid + 1);
        auto rnd = [&] {
            rng ^= rng << 13;
            rng ^= rng >> 7;
            rng ^= rng << 17;
            return rng;
        };
        while (next.fetch_add(1) < count) {
            const u64 off = lo + rnd() % (hi - lo);
            const u64 len = std::min<u64>(hi - off, 1 + rnd() % (16ULL << 20));
            std::vector<u8> got(len);
            romfs->Read(got.data(), len, off);
            // Files overlapping [off, off+len).
            auto it = std::upper_bound(items.begin(), items.end(), off,
                                       [](u64 v, const Item& x) { return v < x.offset; });
            if (it != items.begin()) {
                --it;
            }
            bool ok = true;
            std::string first_bad;
            for (; it != items.end() && it->offset < off + len; ++it) {
                const u64 fs = it->offset, fe = fs + it->file->GetSize();
                if (fe <= off) {
                    continue;
                }
                const u64 s = std::max(fs, off), e = std::min(fe, off + len);
                std::ifstream f(ref_dir + "/" + it->path, std::ios::binary);
                std::vector<u8> ref(e - s);
                f.seekg(static_cast<std::streamoff>(s - fs));
                f.read(reinterpret_cast<char*>(ref.data()), static_cast<std::streamsize>(ref.size()));
                checked_bytes += e - s;
                if (std::memcmp(ref.data(), got.data() + (s - off), e - s) != 0) {
                    if (ok) {
                        u64 k = 0;
                        while (ref[k] == got[s - off + k]) k++;
                        char buf[512];
                        std::snprintf(buf, sizeof(buf), "%s file+0x%llx (romfs 0x%llx)",
                                      it->path.c_str(), static_cast<unsigned long long>(s - fs + k),
                                      static_cast<unsigned long long>(s + k));
                        first_bad = buf;
                    }
                    ok = false;
                }
            }
            if (!ok) {
                bad_reads++;
                std::lock_guard lk{out_mutex};
                std::printf("RAWBAD read off=0x%llx len=0x%llx first mismatch in %s\n",
                            static_cast<unsigned long long>(off),
                            static_cast<unsigned long long>(len), first_bad.c_str());
                std::fflush(stdout);
            }
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < std::max(threads, 1); t++) {
        pool.emplace_back(worker, t);
    }
    for (auto& t : pool) {
        t.join();
    }
    std::printf("RESULT raw_reads=%llu bad_reads=%llu checked_bytes=%llu\n",
                static_cast<unsigned long long>(count), static_cast<unsigned long long>(bad_reads.load()),
                static_cast<unsigned long long>(checked_bytes.load()));
    return bad_reads.load() == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s base.nsp update.nsp refdir [--threads N] [--chunk SIZE] "
                             "[--odd] [--only SUBSTR] [--list]\n", argv[0]);
        return 2;
    }
    const std::string base_path = argv[1], upd_path = argv[2], ref_dir = argv[3];
    int threads = 1;
    u64 chunk = 0;
    bool odd = false, list = false;
    u64 raw = 0;
    std::string only;
    for (int i = 4; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--threads" && i + 1 < argc) threads = std::atoi(argv[++i]);
        else if (a == "--chunk" && i + 1 < argc) chunk = std::strtoull(argv[++i], nullptr, 0);
        else if (a == "--odd") odd = true;
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--list") list = true;
        else if (a == "--raw" && i + 1 < argc) raw = std::strtoull(argv[++i], nullptr, 0);
    }

    Common::Log::Initialize();
    Common::Log::Start();

    auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
    const auto base_file = vfs->OpenFile(base_path, FileSys::OpenMode::Read);
    const auto upd_file = vfs->OpenFile(upd_path, FileSys::OpenMode::Read);
    if (!base_file || !upd_file) {
        std::fprintf(stderr, "cannot open inputs\n");
        return 2;
    }
    FileSys::NSP base_nsp(base_file);
    FileSys::NSP upd_nsp(upd_file);
    const auto base_nca = FindProgram(base_nsp, nullptr, false);
    if (!base_nca || base_nca->GetStatus() != Loader::ResultStatus::Success) {
        std::fprintf(stderr, "no usable base program NCA\n");
        return 2;
    }
    const auto upd_nca = FindProgram(upd_nsp, base_nca.get(), true);
    if (!upd_nca || upd_nca->GetStatus() != Loader::ResultStatus::Success || !upd_nca->GetRomFS()) {
        std::fprintf(stderr, "no usable update program NCA (status %d)\n",
                     upd_nca ? static_cast<int>(upd_nca->GetStatus()) : -1);
        return 2;
    }
    const auto romfs = upd_nca->GetRomFS();
    std::printf("merged romfs size 0x%llx\n", static_cast<unsigned long long>(romfs->GetSize()));
    const auto root = FileSys::ExtractRomFS(romfs);
    if (!root) {
        std::fprintf(stderr, "romfs parse failed\n");
        return 2;
    }
    std::vector<Item> items;
    Collect(root, "", items);
    if (!only.empty()) {
        std::erase_if(items, [&](const Item& it) { return it.path.find(only) == std::string::npos; });
    }
    std::printf("files: %zu, threads %d, chunk %llu, odd %d\n", items.size(), threads,
                static_cast<unsigned long long>(chunk), odd ? 1 : 0);

    if (raw != 0) {
        return RawCheck(romfs, items, ref_dir, raw, threads);
    }

    std::atomic<size_t> next{0};
    std::atomic<size_t> bad{0}, checked{0};
    std::atomic<u64> bytes{0};
    std::mutex out_mutex;

    auto worker = [&](int tid) {
        u64 rng = 0x9E3779B97F4A7C15ULL ^ static_cast<u64>(tid);
        for (;;) {
            const size_t idx = next.fetch_add(1);
            if (idx >= items.size()) {
                return;
            }
            const auto& it = items[idx];
            const u64 size = it.file->GetSize();
            std::vector<u8> got(size);
            if (odd) {
                u64 pos = 0;
                while (pos < size) {
                    rng ^= rng << 13;
                    rng ^= rng >> 7;
                    rng ^= rng << 17;
                    const u64 n = std::min<u64>(size - pos, 1 + (rng % 0x30001));
                    it.file->Read(got.data() + pos, n, pos);
                    pos += n;
                }
            } else if (chunk != 0) {
                for (u64 pos = 0; pos < size; pos += chunk) {
                    it.file->Read(got.data() + pos, std::min<u64>(chunk, size - pos), pos);
                }
            } else if (size != 0) {
                it.file->Read(got.data(), size, 0);
            }
            std::vector<u8> ref;
            {
                std::ifstream f(ref_dir + "/" + it.path, std::ios::binary);
                if (!f) {
                    std::lock_guard lk{out_mutex};
                    std::printf("NOREF %s\n", it.path.c_str());
                    bad++;
                    continue;
                }
                f.seekg(0, std::ios::end);
                ref.resize(static_cast<size_t>(f.tellg()));
                f.seekg(0);
                f.read(reinterpret_cast<char*>(ref.data()), static_cast<std::streamsize>(ref.size()));
            }
            bytes += size;
            checked++;
            if (list) {
                std::lock_guard lk{out_mutex};
                std::printf("FILE 0x%llx 0x%llx %s\n", static_cast<unsigned long long>(it.offset),
                            static_cast<unsigned long long>(size), it.path.c_str());
            }
            if (ref == got) {
                continue;
            }
            bad++;
            // Differing byte ranges.
            std::string ranges;
            u64 ndiff = 0, nranges = 0;
            const u64 common = std::min<u64>(ref.size(), got.size());
            for (u64 i = 0; i < common;) {
                if (ref[i] == got[i]) {
                    i++;
                    continue;
                }
                u64 j = i;
                while (j < common && ref[j] != got[j]) j++;
                // Merge short equal gaps (random equal bytes inside garbage).
                while (j < common) {
                    u64 k = j;
                    while (k < common && ref[k] == got[k] && k - j < 64) k++;
                    if (k < common && k - j < 64 && ref[k] != got[k]) {
                        j = k;
                        while (j < common && ref[j] != got[j]) j++;
                    } else {
                        break;
                    }
                }
                ndiff += j - i;
                if (nranges++ < 12) {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), " [0x%llx,0x%llx)",
                                  static_cast<unsigned long long>(i),
                                  static_cast<unsigned long long>(j));
                    ranges += buf;
                }
                i = j;
            }
            std::lock_guard lk{out_mutex};
            std::printf("DIFF %s romfs_off=0x%llx size=0x%llx ref_size=0x%zx diff_bytes=0x%llx "
                        "ranges=%llu:%s\n",
                        it.path.c_str(), static_cast<unsigned long long>(it.offset),
                        static_cast<unsigned long long>(size), ref.size(),
                        static_cast<unsigned long long>(ndiff),
                        static_cast<unsigned long long>(nranges), ranges.c_str());
            std::fflush(stdout);
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < std::max(threads, 1); t++) {
        pool.emplace_back(worker, t);
    }
    for (auto& t : pool) {
        t.join();
    }
    std::printf("RESULT checked=%zu bad=%zu bytes=%llu\n", checked.load(), bad.load(),
                static_cast<unsigned long long>(bytes.load()));
    return bad.load() == 0 ? 0 : 1;
}
