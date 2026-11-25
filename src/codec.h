#pragma once

#include <QString>

// Which codec the app asks YouTube for, and why it asks for it.
//
// The reason this exists: on a GPU whose VA-API driver decodes H.264 and nothing
// newer (Haswell and Broadwell -- no VP9, no AV1), the plain `bestvideo` hands
// back AV1 or VP9. That GPU cannot decode it, so mpv quietly drops to software,
// and playback then *looks fine* while dropping frames and cooking the CPU. That
// is the worst shape a failure can take: silent, and easy to blame on the network.
// Asking for H.264 keeps the hardware path that the machine actually has.
//
// `auto` is decided from VA-API's own profile list (`vainfo`), which is the only
// honest source -- the driver saying what it can decode. When vainfo gives no
// answer the app does NOT guess from the CPU model or the iGPU's PCI id: such a
// guess would silently change which codec every video is served in, and the app
// would be attributing to the hardware something it never checked. It leaves the
// codec to YouTube and says "unknown".
namespace Codec {

enum class Preference {
    Auto,  // decide from what this machine can really decode
    Avc,   // H.264 -- the only hardware path on the pre-VP9 generations
    Vp9,
    Av1,
};

// The `codec = ...` value from config.toml. Anything unrecognised -- including a
// misspelling -- is Auto, never an error: that file is read-only and a bad word
// in it must not be able to stop playback.
Preference fromConfig(const QString &value);

// "auto" / "avc" / "vp9" / "av1".
QString name(Preference preference);

// What the driver can decode in hardware (VLD = decode; a profile advertised for
// encoding only does not count).
struct Capability {
    bool h264 = false;
    bool vp9 = false;
    bool av1 = false;
    // Which VA-API driver answered: "i965" (the legacy pre-Skylake path), "iHD",
    // or empty when unknown. Only i965 is acted on -- see effective().
    QString driver;
    // Where the answer came from: "vainfo", or empty when there was no answer.
    QString source;
    bool any() const { return h264 || vp9 || av1; }
};

// Parse vainfo's output. Split out from probe() so the decision can be tested
// against a Haswell machine's output without owning one.
Capability fromVainfo(const QString &text);

// Ask this machine. Runs vainfo; never guesses.
Capability probe();

// Auto resolved against what the machine can decode: when H.264 is the only
// hardware path, Auto means H.264. An explicit preference is always honoured
// as-is, even where hardware cannot decode it -- that is the user overriding the
// app on purpose, and the footer reports what actually happens.
//
// On the legacy i965 driver, Auto means H.264 EVEN IF VP9 is advertised: the VP9
// that driver offers on pre-Skylake parts is hybrid, with part of the work still
// on the CPU, so a fully hardware H.264 stream is the better answer there. The
// user confirmed this on a Devil's Canyon. This is a driver-derived rule, not a
// CPU-model table: it acts on what the driver calls itself, nothing else.
Preference effective(Preference configured, const Capability &capability);

// V in the window: auto -> avc -> vp9 -> av1 -> auto.
Preference next(Preference preference);

// The yt-dlp format expression. Whatever the preference, the expression ALWAYS
// ends in an unfiltered fallback, because plenty of videos have no stream in the
// wanted codec at all (measured: a 1080p60 H.264 stream may not exist where a
// 1080p30 one does, and some videos are VP9/AV1 only). A codec preference is a
// preference, never a demand that makes a video unplayable.
QString selector(Preference effectivePreference, bool audioOnly);

// One line saying what will be asked for and why, from the driver's answer.
QString describe(Preference configured, const Capability &capability);

// The same decision in a few words, for the footer: "auto", "auto → H.264",
// "H.264" or "VP9 (software)". The explanation lives in describe(), which is what
// --status and the key list show.
QString summary(Preference configured, const Capability &capability);

}  // namespace Codec
