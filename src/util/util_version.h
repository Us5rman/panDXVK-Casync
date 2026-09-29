#pragma once

#include <string>

#include <version.h>
#include <pandxvk_version.h>

namespace dxvk::util {

  /**
   * \brief panDXVK release string, e.g. "1.10.3.v8"
   *
   * Derived from DXVK_VERSION so it tracks the release tag with no manual
   * bump: meson fills DXVK_VERSION from `git describe --tags`, and the
   * nearest tag is exactly the release being shipped.
   *
   *   v1.10.3-panVK.8-6-g2c575a3  ->  1.10.3.v8
   *   v1.10.3-panVK.8             ->  1.10.3.v8   (built on the tag itself)
   *   v1.10.3-panVK.9+            ->  1.10.3.v9   (dirty build; "+" dropped)
   *   v1.10.3-panVK.10            ->  1.10.3.v10  (multi-digit release)
   *
   * The commit-count and hash tail are deliberately dropped: the release
   * string stays pinned to the last tag until the next one is cut, so a
   * build made six commits after panVK.8 still reports ".v8".
   *
   * Only the leading digits after "panVK." are taken. That matters when a
   * build sits exactly on a tag, where git describe has no "-N-g" tail to
   * cut at and the whole remainder would otherwise be used - including the
   * dirty marker `--dirty=+` appends, which would report "1.10.3.v9+".
   *
   * Falls back to DXVK_VERSION verbatim when the tag shape is not
   * recognised (for example when git describe failed at build time), and
   * to the release on its own when no release number follows "panVK.".
   */
  inline std::string panDxvkRelease() {
    const std::string version = DXVK_VERSION;

    const size_t tagPos = version.find("-panVK.");
    if (tagPos == std::string::npos)
      return version;

    std::string release = version.substr(0, tagPos);
    if (!release.empty() && release[0] == 'v')
      release.erase(0, 1);

    const std::string tail = version.substr(tagPos + 7);

    size_t digits = 0;
    while (digits < tail.size() && tail[digits] >= '0' && tail[digits] <= '9')
      ++digits;

    if (digits == 0)
      return release;

    return release + ".v" + tail.substr(0, digits);
  }


  /**
   * \brief Release and commit, e.g. "1.10.3.v8 (2c575a3)"
   *
   * One string carrying both halves: the stable release identifier for
   * humans, and the short hash so a report can be traced back to an exact
   * commit in one step. Used verbatim by the HUD and by the startup log
   * line; callers supply their own "panDXVK" label.
   */
  inline std::string panDxvkVersionString() {
    return panDxvkRelease() + " (" + PAN_DXVK_COMMIT + ")";
  }

}
