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
Version:        0.1.1
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
