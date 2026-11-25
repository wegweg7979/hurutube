#include "codec.h"

#include <QProcess>
#include <QStringList>

namespace Codec {
namespace {

// VLD is decoding. A driver can advertise the same profile for encoding only
// (Haswell does for H.264), which says nothing about playback.
bool decodes(const QStringList &lines, const QString &profile)
{
    for (const QString &line : lines) {
        if (line.contains(QStringLiteral("VAEntrypointVLD")) && line.contains(profile))
            return true;
    }
    return false;
}

QStringList decodable(const Capability &capability)
{
    QStringList list;
    if (capability.h264)
        list << QStringLiteral("H.264");
    if (capability.vp9)
        list << QStringLiteral("VP9");
    if (capability.av1)
        list << QStringLiteral("AV1");
    return list;
}

// "H.264, VP9 and AV1" -- the only place a list is humanised, so the two callers
// that need it cannot drift apart.
QString humanList(const QStringList &parts)
{
    if (parts.size() < 2)
        return parts.join(QString());
    return parts.mid(0, parts.size() - 1).join(QStringLiteral(", ")) + QStringLiteral(" and ")
           + parts.last();
}

// The codec an explicit preference names, for the "cannot decode that here" note.
bool isDecodable(Preference preference, const Capability &capability)
{
    switch (preference) {
    case Preference::Avc:
        return capability.h264;
    case Preference::Vp9:
        return capability.vp9;
    case Preference::Av1:
        return capability.av1;
    case Preference::Auto:
        break;
    }
    return false;
}

}  // namespace

Preference fromConfig(const QString &value)
{
    const QString cleaned = value.trimmed().toLower();
    if (cleaned == QStringLiteral("avc") || cleaned == QStringLiteral("h264"))
        return Preference::Avc;
    if (cleaned == QStringLiteral("vp9"))
        return Preference::Vp9;
    if (cleaned == QStringLiteral("av1"))
        return Preference::Av1;
    // "auto", an empty value, or anything misspelled. Deliberately the same
    // answer for all three: config.toml is read-only, so a typo in it can never
    // be fixed by the app, and it must not be able to stop playback either.
    return Preference::Auto;
}

QString name(Preference preference)
{
    switch (preference) {
    case Preference::Avc:
        return QStringLiteral("avc");
    case Preference::Vp9:
        return QStringLiteral("vp9");
    case Preference::Av1:
        return QStringLiteral("av1");
    case Preference::Auto:
        break;
    }
    return QStringLiteral("auto");
}

Capability fromVainfo(const QString &text)
{
    const QStringList lines = QString::fromUtf8(text.toUtf8()).split(QLatin1Char('\n'));
    Capability capability;
    capability.h264 = decodes(lines, QStringLiteral("VAProfileH264High"))
                      || decodes(lines, QStringLiteral("VAProfileH264Main"))
                      || decodes(lines, QStringLiteral("VAProfileH264ConstrainedBaseline"))
                      || decodes(lines, QStringLiteral("VAProfileH264Baseline"));
    capability.vp9 = decodes(lines, QStringLiteral("VAProfileVP9Profile0"));
    capability.av1 = decodes(lines, QStringLiteral("VAProfileAV1Profile0"));
    if (capability.any())
        capability.source = QStringLiteral("vainfo");
    return capability;
}

Capability probe()
{
    QProcess vainfo;
    vainfo.start(QStringLiteral("vainfo"), {});
    if (vainfo.waitForFinished(4000)) {
        const Capability capability = fromVainfo(QString::fromUtf8(vainfo.readAllStandardOutput()));
        if (capability.any())
            return capability;
    }
    // No answer (no vainfo, no render device, an error). Left empty on purpose --
    // see the note at the top of codec.h about not inferring this from a model
    // number. source stays empty, and describe() says so.
    return Capability{};
}

Preference effective(Preference configured, const Capability &capability)
{
    if (configured != Preference::Auto)
        return configured;
    if (!capability.any())
        return Preference::Auto;
    // H.264 alone means every other codec would be software. Only for the case
    // where the driver has told us that.
    if (!capability.vp9 && !capability.av1)
        return Preference::Avc;
    return Preference::Auto;
}

Preference next(Preference preference)
{
    switch (preference) {
    case Preference::Auto:
        return Preference::Avc;
    case Preference::Avc:
        return Preference::Vp9;
    case Preference::Vp9:
        return Preference::Av1;
    case Preference::Av1:
        return Preference::Auto;
    }
    return Preference::Auto;
}

QString selector(Preference effectivePreference, bool audioOnly)
{
    if (audioOnly)
        return QStringLiteral("bestaudio/best");

    // 1080p keeps VA-API comfortable on this class of GPU while staying ahead of
    // any screen the user has. Every branch ends in an unfiltered fallback: the
    // preference must never make a video unplayable.
    const QString cap = QStringLiteral("height<=1080");
    const QString fallback = QStringLiteral("bestvideo[%1]+bestaudio/best[%1]").arg(cap);
    switch (effectivePreference) {
    case Preference::Avc:
        // ^avc covers avc1.* ; the regex form also survives a future spelling.
        return QStringLiteral("bestvideo[vcodec~=\"^avc\"][%1]+bestaudio/%2").arg(cap, fallback);
    case Preference::Vp9:
        // ^vp0?9 covers vp9 and vp09.*
        return QStringLiteral("bestvideo[vcodec~=\"^vp0?9\"][%1]+bestaudio/%2").arg(cap, fallback);
    case Preference::Av1:
        // ^av0?1 covers av1 and av01.*
        return QStringLiteral("bestvideo[vcodec~=\"^av0?1\"][%1]+bestaudio/%2").arg(cap, fallback);
    case Preference::Auto:
        break;
    }
    return fallback;
}

QString describe(Preference configured, const Capability &capability)
{
    const Preference use = effective(configured, capability);

    if (configured != Preference::Auto) {
        QString text = QStringLiteral("%1 (asked for in codec = %2)")
                           .arg(use == Preference::Avc ? QStringLiteral("H.264")
                                                       : use == Preference::Vp9 ? QStringLiteral("VP9")
                                                                                : QStringLiteral("AV1"),
                                name(configured));
        // Overriding the app on purpose is allowed; being quiet about the cost is
        // not. The footer separately reports what mpv actually ended up using.
        if (capability.any() && !isDecodable(use, capability)) {
            text += QStringLiteral(" — this GPU has no %1 VLD, so it will decode on the CPU")
                        .arg(use == Preference::Vp9 ? QStringLiteral("VP9") : QStringLiteral("AV1"));
        }
        return text;
    }

    if (!capability.any())
        return QStringLiteral("auto (VA-API gave no answer — the codec is left to YouTube)");

    const QStringList can = decodable(capability);
    if (use == Preference::Avc)
        return QStringLiteral("auto → H.264 (VA-API here decodes %1 only, so AV1/VP9 would run on "
                              "the CPU)")
            .arg(humanList(can));
    return QStringLiteral("auto (VA-API here decodes %1 — nothing to restrict)").arg(humanList(can));
}

QString summary(Preference configured, const Capability &capability)
{
    const Preference use = effective(configured, capability);
    if (use == Preference::Auto)
        return QStringLiteral("auto");

    const QString wanted = use == Preference::Avc   ? QStringLiteral("H.264")
                           : use == Preference::Vp9 ? QStringLiteral("VP9")
                                                    : QStringLiteral("AV1");
    if (configured == Preference::Auto)
        return QStringLiteral("auto → %1").arg(wanted);  // the machine decided this
    if (capability.any() && !isDecodable(use, capability))
        return QStringLiteral("%1 (software)").arg(wanted);
    return wanted;
}

}  // namespace Codec
