#pragma once

namespace localvault {

// The caller holds the repository OS lock and retains a normal database connection.
// This VFS keeps native shared locks while enabling a private, read-only WAL index.
[[nodiscard]] const char* locked_read_vfs_name();

} // namespace localvault
