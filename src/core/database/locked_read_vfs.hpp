#pragma once

namespace localvault {

// The caller retains the exclusive repository OS lock through database close.
// Native shared locks protect WAL lifetime while this VFS uses a private read-only index.
[[nodiscard]] const char* locked_read_vfs_name();

} // namespace localvault
