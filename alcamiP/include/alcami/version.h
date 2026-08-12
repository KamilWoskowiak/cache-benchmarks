/// \file
///
/// Version of the ALCaMI library.

#ifndef ALCAMI_INCLUDE_VERSION_H
#define ALCAMI_INCLUDE_VERSION_H

#include <string_view>

#include <alcami/config.h>

namespace alc {

/// \addtogroup misc
/// \{

/// Version string.
inline constexpr std::string_view version_string{ALCAMI_VERSION};

/// Major version.
inline constexpr int major_version{ALCAMI_VERSION_MAJOR};

/// Minor version.
inline constexpr int minor_version{ALCAMI_VERSION_MINOR};

/// Patch version.
inline constexpr int patch_version{ALCAMI_VERSION_PATCH};

/// \}

} // namespace alc

#endif // ALCAMI_INCLUDE_VERSION_H
