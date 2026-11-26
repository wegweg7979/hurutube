# hurutube.spec — a small YouTube player: search, watch, and no Shorts.
#
# hurutube (https://github.com/wegweg7979/hurutube, MIT) is a plain C++17
# CMake/Qt6 app: `bin/build` is the build, `pkgbuild/` the launcher entry and
# icon — the same shape as huruwave.spec and airgraph.spec in their own COPRs.
#
# COPR-safe: every dependency is a BuildRequires and the build needs no network.
# Playback happens in mpv, which resolves YouTube URLs through yt-dlp at
# *runtime*, which is why those are Requires and not BuildRequires.

# The release build produces no debug info, so an empty debuginfo subpackage
# would fail the build.
%global debug_package %{nil}

Name:           hurutube
Version:        0.1.5
Release:        1%{?dist}
Summary:        A small YouTube player: search, watch, and no Shorts

License:        MIT
URL:            https://github.com/wegweg7979/hurutube

# GitHub's tag archive unpacks to hurutube-<version>/. bin/spec-sources fetches
# it and rewrites hurutube.spec.sources with its sha256 — after the tag exists,
# which is why the hash is a step of its own and not part of the version bump.
Source0:        %{url}/archive/refs/tags/v%{version}/%{name}-%{version}.tar.gz

ExclusiveArch:  x86_64

BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  desktop-file-utils
# CMakeLists asks for Qt6 Core/Gui/Qml/Quick/QuickControls2/Network/Test. Qt Quick
# Controls — including the Material style the window sets — ships inside
# qt6-qtdeclarative on Fedora 44/45; there is no qt6-qtquickcontrols2 package any
# more. QtTest comes with qt6-qtbase-devel.
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qtdeclarative-devel

# Runtime, not linked. mpv is the playback engine over its JSON IPC socket and it
# resolves YouTube URLs through yt-dlp itself, so both are hard requirements: the
# app is useless without them and neither is a build dependency. The Wayland
# platform plugin is not optional on a Hyprland-only image either.
Requires:       mpv
Requires:       yt-dlp
Requires:       qt6-qtbase
Requires:       qt6-qtdeclarative
Requires:       qt6-qtwayland

%description
hurutube searches YouTube, lists channels and playlists, and plays what you pick
in mpv — which decodes on the GPU through VA-API. The window itself is a browser
and a remote control; the video opens in mpv's own window, full screen when you
want it.

Shorts are blocked on purpose, and structurally: YouTube's own search API returns
Shorts in a renderer of their own, so they are never read into the list at all
rather than filtered out afterwards, and a /shorts/ link is refused instead of
played. Channel listings come from the /videos tab, which carries none either.

Everything else is deliberately absent — no download button, no thumbnail cache,
no account, no shorts, no feed. What was watched is remembered locally with its
position, so the next play resumes where you stopped. Every path is also
reachable from a shell (`hurutube --search … --no-window`, `--history`,
`--status`).

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -DCMAKE_BUILD_TYPE=Release
%cmake_build

%install
install -D -m 0755 %{_vpath_builddir}/hurutube %{buildroot}%{_bindir}/hurutube
install -D -m 0644 pkgbuild/hurutube.svg \
  %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/hurutube.svg
install -D -m 0644 pkgbuild/hurutube.desktop \
  %{buildroot}%{_datadir}/applications/hurutube.desktop

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/hurutube.desktop
# Offscreen, no display, no audio device and no network — CMakeLists already pins
# QT_QPA_PLATFORM=offscreen on the test — so a release that fails its own tests
# does not publish.
%ctest

%files
%license LICENSE
%doc README.md
%{_bindir}/hurutube
%{_datadir}/icons/hicolor/scalable/apps/hurutube.svg
%{_datadir}/applications/hurutube.desktop

%changelog
* Wed Nov 25 2026 wegweg7979 <diode-crumble-0h@icloud.com> - 0.1.5-1
- The help screen carries the wordmark, compiled into the binary as a resource
  rather than read from a file beside it, so it cannot go missing in packaging.
- `auto` no longer takes the legacy Intel driver (i965) at its word: that driver
  serves pre-Skylake parts where an advertised VP9 decode is hybrid -- part of the
  work stays on the CPU -- so fully hardware H.264 is chosen instead. The two
  reasons are stated separately in --status and the footer: "only partly
  accelerated" when the newer codec is advertised, "decodes H.264 only" when it is
  not. Driver-derived, never a CPU-model table; iHD (Skylake and later) is
  untouched.
- Verified against real payloads: a capture from a Devil's Canyon (i5-4690K, i965,
  no VP9/AV1 advertised at all) and a constructed i965-with-VP9 one. 27 tests.
* Wed Nov 25 2026 wegweg7979 <diode-crumble-0h@icloud.com> - 0.1.4-1
- Ask for a codec this machine can actually decode. `codec = auto|avc|vp9|av1` in
  config.toml, and V cycles it for the session. `auto` is decided from VA-API's own
  profile list: a GPU that decodes H.264 and nothing newer (Haswell, Broadwell) now
  asks YouTube for H.264 instead of being handed AV1/VP9 to decode on the CPU --
  which looks like working playback while it drops frames. With no answer from the
  driver nothing is restricted and the app says so rather than guessing from a CPU
  model or a PCI id. Nothing is written back to config.toml (read-only on purpose).
- A codec preference is never a demand: every format expression ends in an
  unfiltered fallback, so a video with no stream in the wanted codec still plays.
- `--status` prints which codec will be asked for and why; the window footer shows
  it tersely, and "software" when a preference cannot be decoded in hardware here.
- A running engine that ignores SIGTERM is now killed rather than waited on, when
  the cookie source or the codec preference changes -- otherwise the app was left
  with no engine at all (measured once: a socket refusing connections while an mpv
  process still existed).
* Wed Nov 25 2026 wegweg7979 <diode-crumble-0h@icloud.com> - 0.1.3-1
- The list answers the mouse: a single click moves the selection, and a double
  click plays the row under the cursor. The previous double-click handler could
  never fire at all -- its MouseArea accepted no buttons -- and it played the
  highlighted row rather than the clicked one.
- `Space` acts on what is pointed at: when the highlighted row is not the video
  that is playing it starts that row, and when it is, it pauses or resumes. Before,
  Space paused whenever any file was loaded, so pressing it on another row paused
  the video that was already running.
- The playback engine is started AFTER the cookie source is set. setCookieSource()
  terminates a running engine when the source changes, and the engine used to start
  in the constructor -- so on a machine where a cookie source resolves (Firefox
  here), every launch killed the engine it had just started, and when mpv had not
  finished dying inside the wait the restart was skipped, leaving the app with an
  IPC socket that accepted no connections. Nothing needs an engine before the
  window is loaded, so the headless commands no longer start one either.
* Wed Nov 25 2026 wegweg7979 <diode-crumble-0h@icloud.com> - 0.1.2-1
- The YouTube session is now resolved rather than merely configured: an explicit
  `cookies_file` or `cookies_from`, then a cookies file that really carries a
  session, then Firefox, then another player's setting -- and the window and
  `--status` say what it can actually do ("signed in -- cookie file, 24 cookies,
  6 of them a session" against "no cookies -- anonymous search only").
- Cookies stay a FALLBACK, never the default, on measurement: a signed-in session
  offers fewer formats and a lower bitrate at every resolution (2160p 27987 to
  17174 kbps, 1080p 4534 to 816), the same throughput on the same stream, and
  about 3.5 s longer before playback starts. The "Premium" 1080p stream is not
  cookie-gated either -- the app's own selector picks it anonymously.
- Space plays the highlighted row as well as pausing and resuming it, and the
  arrow keys are ten seconds in the video window too: mpv's built-in bindings are
  five seconds and are the ones that fire while a video is up.
- A helper process that will not die no longer drops the request. Qt's `start()`
  on a process still reading Running does nothing but warn, which silently cost
  the pre-resolve (playback fell back to mpv's own extraction) or a listing;
  the request is now deferred until the process is really gone, and refusals are
  reported with a name.
* Tue Nov 24 2026 wegweg7979 <diode-crumble-0h@icloud.com> - 0.1.1-1
- The headless commands (`--status`, `--history`, `--search … --no-window`) no
  longer abort without a display. They constructed a QGuiApplication, which needs
  a platform plugin and core-dumps when there is none, so the packaged
  `hurutube --status` died in a container while working perfectly in a terminal
  on the desktop -- a terminal there still has a session, which is what hid it.
  The application class is now chosen from the arguments: the window needs
  QGuiApplication, everything else only QCoreApplication.
- `--history` prints and exits on its own, instead of silently doing nothing
  unless it was paired with --no-window.
* Tue Nov 24 2026 wegweg7979 <diode-crumble-0h@icloud.com> - 0.1.0-1
- First release: search through YouTube's inner API (sub-second, and Shorts are
  never read into the list because they arrive in their own renderer), channel
  and playlist listings through yt-dlp, and playback through mpv with VA-API
  hardware decode — measured on studiobox2, AV1 1080p decoded on the GPU.
- The selected row is pre-resolved, so playback starts in about a second instead
  of the ~18s a cold resolve inside the playback path costs.
- Resume with position, an audio-only mode, a watched list (Ctrl+H) and the
  headless commands for scripts and agents.
