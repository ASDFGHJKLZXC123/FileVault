#pragma once

#include <array>

namespace localvault {

enum class FailurePoint {
    after_temp_object_write,
    after_object_fsync,
    after_object_rename,
    before_metadata_batch_commit,
    before_snapshot_publish,
    during_restore_write,
    after_gc_object_delete,
};

inline constexpr std::array<FailurePoint, 7> all_failure_points{
    FailurePoint::after_temp_object_write, FailurePoint::after_object_fsync,
    FailurePoint::after_object_rename,     FailurePoint::before_metadata_batch_commit,
    FailurePoint::before_snapshot_publish, FailurePoint::during_restore_write,
    FailurePoint::after_gc_object_delete,
};

class FailureInjector {
  public:
    virtual ~FailureInjector() noexcept = default;
    virtual void hit(FailurePoint point) = 0;
};

} // namespace localvault
