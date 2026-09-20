#include "database/locked_read_vfs.hpp"

#include <sqlite3.h>

#include <limits>
#include <new>

#include "localvault/error.hpp"

namespace localvault {
namespace {

struct LockedFile {
    sqlite3_file base{};
    sqlite3_file* native{};
    int native_lock{SQLITE_LOCK_NONE};
};

LockedFile& wrapped(sqlite3_file* file) noexcept {
    return *reinterpret_cast<LockedFile*>(file);
}

sqlite3_file* native(sqlite3_file* file) noexcept {
    return wrapped(file).native;
}

int close_file(sqlite3_file* file) noexcept {
    const int result = native(file)->pMethods->xClose(native(file));
    file->pMethods = nullptr;
    return result;
}

int read_file(sqlite3_file* file, void* buffer, int size, sqlite3_int64 offset) noexcept {
    return native(file)->pMethods->xRead(native(file), buffer, size, offset);
}

int write_file(sqlite3_file*, const void*, int, sqlite3_int64) noexcept {
    return SQLITE_READONLY;
}

int truncate_file(sqlite3_file*, sqlite3_int64) noexcept {
    return SQLITE_READONLY;
}

int sync_file(sqlite3_file*, int) noexcept {
    return SQLITE_READONLY;
}

int file_size(sqlite3_file* file, sqlite3_int64* size) noexcept {
    return native(file)->pMethods->xFileSize(native(file), size);
}

int lock_file(sqlite3_file* file, int level) noexcept {
    if (level == SQLITE_LOCK_EXCLUSIVE) {
        // Only the pager's EXCLUSIVE upgrade is synthetic. Native SHARED locking
        // and native close accounting must remain intact (especially POSIX fcntl).
        return wrapped(file).native_lock >= SQLITE_LOCK_SHARED ? SQLITE_OK : SQLITE_IOERR_LOCK;
    }
    const int result = native(file)->pMethods->xLock(native(file), level);
    if (result == SQLITE_OK) {
        wrapped(file).native_lock = level;
    }
    return result;
}

int unlock_file(sqlite3_file* file, int level) noexcept {
    const int result = native(file)->pMethods->xUnlock(native(file), level);
    if (result == SQLITE_OK) {
        wrapped(file).native_lock = level;
    }
    return result;
}

int check_reserved(sqlite3_file* file, int* held) noexcept {
    return native(file)->pMethods->xCheckReservedLock(native(file), held);
}

int file_control(sqlite3_file* file, int operation, void* argument) noexcept {
    return native(file)->pMethods->xFileControl(native(file), operation, argument);
}

int sector_size(sqlite3_file* file) noexcept {
    return native(file)->pMethods->xSectorSize(native(file));
}

int device_characteristics(sqlite3_file* file) noexcept {
    return native(file)->pMethods->xDeviceCharacteristics(native(file));
}

int shm_map(sqlite3_file*, int, int, int, void volatile**) noexcept {
    return SQLITE_IOERR_SHMMAP;
}

int shm_lock(sqlite3_file*, int, int, int) noexcept {
    return SQLITE_IOERR_SHMLOCK;
}

void shm_barrier(sqlite3_file*) noexcept {}

int shm_unmap(sqlite3_file*, int) noexcept {
    return SQLITE_OK;
}

int fetch_file(sqlite3_file* file, sqlite3_int64 offset, int size, void** buffer) noexcept {
    const auto* methods = native(file)->pMethods;
    if (methods->iVersion >= 3 && methods->xFetch != nullptr) {
        return methods->xFetch(native(file), offset, size, buffer);
    }
    *buffer = nullptr;
    return SQLITE_OK;
}

int unfetch_file(sqlite3_file* file, sqlite3_int64 offset, void* buffer) noexcept {
    const auto* methods = native(file)->pMethods;
    return methods->iVersion >= 3 && methods->xUnfetch != nullptr
               ? methods->xUnfetch(native(file), offset, buffer)
               : SQLITE_OK;
}

const sqlite3_io_methods read_methods{
    3,
    close_file,
    read_file,
    write_file,
    truncate_file,
    sync_file,
    file_size,
    lock_file,
    unlock_file,
    check_reserved,
    file_control,
    sector_size,
    device_characteristics,
    shm_map,
    shm_lock,
    shm_barrier,
    shm_unmap,
    fetch_file,
    unfetch_file,
};

struct Registration {
    sqlite3_vfs vfs{};
    sqlite3_vfs* parent{};

    Registration();
};

int open_file(sqlite3_vfs* vfs, sqlite3_filename name, sqlite3_file* file, int flags,
              int* output_flags) noexcept {
    auto* parent = reinterpret_cast<Registration*>(vfs)->parent;
    if ((flags & (SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_WAL)) == 0) {
        // Bounded-memory SQL sorts may spill to anonymous, disposable scratch files.
        if (name == nullptr && (flags & SQLITE_OPEN_DELETEONCLOSE) != 0) {
            return parent->xOpen(parent, name, file, flags, output_flags);
        }
        return SQLITE_READONLY;
    }
    auto* wrapper = new (file) LockedFile{};
    wrapper->native = reinterpret_cast<sqlite3_file*>(wrapper + 1);
    wrapper->native->pMethods = nullptr;
    flags &= ~(SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_DELETEONCLOSE);
    flags |= SQLITE_OPEN_READONLY;
    const int result = parent->xOpen(parent, name, wrapper->native, flags, output_flags);
    if (result == SQLITE_OK) {
        wrapper->base.pMethods = &read_methods;
    } else if (wrapper->native->pMethods != nullptr) {
        (void)wrapper->native->pMethods->xClose(wrapper->native);
    }
    return result;
}

int delete_file(sqlite3_vfs*, const char*, int) noexcept {
    return SQLITE_READONLY;
}

Registration::Registration() : parent(sqlite3_vfs_find(nullptr)) {
    if (parent == nullptr || parent->szOsFile > (std::numeric_limits<int>::max)() -
                                                    static_cast<int>(sizeof(LockedFile))) {
        throw LocalVaultError(ErrorCode::database_error, "SQLite native VFS is unavailable");
    }
    vfs = *parent;
    vfs.pNext = nullptr;
    vfs.zName = "localvault-locked-read";
    vfs.szOsFile += static_cast<int>(sizeof(LockedFile));
    vfs.xOpen = open_file;
    vfs.xDelete = delete_file;
    if (sqlite3_vfs_register(&vfs, 0) != SQLITE_OK) {
        throw LocalVaultError(ErrorCode::database_error, "could not register locked-read VFS");
    }
}

} // namespace

const char* locked_read_vfs_name() {
    // C++ static initialization serializes registration across threads. Callbacks
    // only delegate C operations and are noexcept; registration errors stay here.
    static Registration registration;
    return registration.vfs.zName;
}

} // namespace localvault
