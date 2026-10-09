#!/usr/bin/env bash
# ==============================================================================
# DeaDBeeF Cover Flow - Debian (.deb) Packaging Script
# 
# Builds and packages DeaDBeeF with Cover Flow into 'deadbeef-coverflow'
# to prevent any conflicts with the system's official 'deadbeef' package.
# Target: Linux Mint 22.3 / Ubuntu 24.04 (noble, amd64)
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PACKAGE_NAME="deadbeef-coverflow"
RAW_VERSION="1.10.3"

# --- 1. Detect OS Distribution Tag ---
DIST_TAG="mint22.3"
if [ -f /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    if [ "${ID:-}" = "linuxmint" ] && [ -n "${VERSION_ID:-}" ]; then
        DIST_TAG="mint${VERSION_ID}"
    elif [ -n "${UBUNTU_CODENAME:-}" ]; then
        DIST_TAG="${UBUNTU_CODENAME}"
    elif [ -n "${VERSION_CODENAME:-}" ]; then
        DIST_TAG="${VERSION_CODENAME}"
    fi
fi

VERSION="${RAW_VERSION}-1~${DIST_TAG}"
ARCH="$(dpkg --print-architecture 2>/dev/null || uname -m | sed 's/x86_64/amd64/')"
OUTPUT_DEB="${SCRIPT_DIR}/${PACKAGE_NAME}_${VERSION}_${ARCH}.deb"
STAGING_DIR="/tmp/deadbeef-deb-staging-$$"

echo "===================================================================="
echo " Building: ${PACKAGE_NAME} (${VERSION}) [${ARCH}]"
echo " Target OS: ${PRETTY_NAME:-Linux} (Tag: ${DIST_TAG})"
echo " Output   : ${OUTPUT_DEB}"
echo "===================================================================="

# --- 2. Check Build Prerequisites ---
echo "[1/6] Checking build prerequisites..."
REQUIRED_TOOLS=("gcc" "make" "pkg-config" "dpkg-deb")
for tool in "${REQUIRED_TOOLS[@]}"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: Required tool '$tool' is not installed." >&2
        exit 1
    fi
done

if ! pkg-config --exists gtk+-3.0; then
    echo "ERROR: libgtk-3-dev is required but not installed." >&2
    exit 1
fi

if ! pkg-config --exists epoxy; then
    echo "ERROR: libepoxy-dev is required but not installed." >&2
    exit 1
fi
echo "All build tools found."

# --- 3. Build Cover Flow Plugin ---
echo "[2/6] Compiling Cover Flow GTK3 OpenGL Plugin..."
make -C "${SCRIPT_DIR}/plugins/coverflow" clean
make -C "${SCRIPT_DIR}/plugins/coverflow"

COVERFLOW_SO="${SCRIPT_DIR}/plugins/coverflow/coverflow_gtk3.so"
if [ ! -f "${COVERFLOW_SO}" ]; then
    echo "ERROR: Cover Flow plugin binary (${COVERFLOW_SO}) was not generated!" >&2
    exit 1
fi
echo "Cover Flow plugin compiled successfully."

# --- 4. Locate DeaDBeeF Base Binaries & Plugins ---
echo "[3/6] Locating DeaDBeeF 1.10.3 base binaries..."
BASE_SRC=""
if [ -d "/opt/deadbeef-1.10.3" ] && [ -x "/opt/deadbeef-1.10.3/deadbeef" ]; then
    BASE_SRC="/opt/deadbeef-1.10.3"
elif [ -d "/opt/deadbeef" ] && [ -x "/opt/deadbeef/deadbeef" ]; then
    BASE_SRC="/opt/deadbeef"
fi

if [ -z "${BASE_SRC}" ]; then
    echo "ERROR: Could not find DeaDBeeF base runtime in /opt/deadbeef-1.10.3 or /opt/deadbeef." >&2
    exit 1
fi
echo "Using DeaDBeeF base from: ${BASE_SRC}"

# --- 5. Prepare Staging Directory ---
echo "[4/6] Preparing staging directory: ${STAGING_DIR}..."
rm -rf "${STAGING_DIR}"
mkdir -p "${STAGING_DIR}/opt/${PACKAGE_NAME}"
mkdir -p "${STAGING_DIR}/usr/bin"
mkdir -p "${STAGING_DIR}/usr/share/applications"
mkdir -p "${STAGING_DIR}/usr/share/pixmaps"
mkdir -p "${STAGING_DIR}/DEBIAN"

# Copy runtime base into /opt/deadbeef-coverflow to avoid any conflict
cp -a "${BASE_SRC}/deadbeef" "${STAGING_DIR}/opt/${PACKAGE_NAME}/"
if [ -f "${BASE_SRC}/deadbeef.png" ]; then
    cp -a "${BASE_SRC}/deadbeef.png" "${STAGING_DIR}/opt/${PACKAGE_NAME}/"
fi
cp -a "${BASE_SRC}/lib" "${STAGING_DIR}/opt/${PACKAGE_NAME}/"
cp -a "${BASE_SRC}/plugins" "${STAGING_DIR}/opt/${PACKAGE_NAME}/"
cp -a "${BASE_SRC}/pixmaps" "${STAGING_DIR}/opt/${PACKAGE_NAME}/"
cp -a "${BASE_SRC}/locale" "${STAGING_DIR}/opt/${PACKAGE_NAME}/"
if [ -d "${BASE_SRC}/doc" ]; then
    cp -a "${BASE_SRC}/doc" "${STAGING_DIR}/opt/${PACKAGE_NAME}/"
fi

# Copy newly built Cover Flow plugin
cp -f "${COVERFLOW_SO}" "${STAGING_DIR}/opt/${PACKAGE_NAME}/plugins/coverflow_gtk3.so"

# Build and copy updated notify plugin
if [ -d "${SCRIPT_DIR}/plugins/notify" ]; then
    make -C "${SCRIPT_DIR}/plugins/notify" clean
    make -C "${SCRIPT_DIR}/plugins/notify"
    if [ -f "${SCRIPT_DIR}/plugins/notify/notify.so" ]; then
        cp -f "${SCRIPT_DIR}/plugins/notify/notify.so" "${STAGING_DIR}/opt/${PACKAGE_NAME}/plugins/notify.so"
    fi
fi

# Compile native C launcher for /usr/bin/deadbeef-coverflow
echo "Compiling native C launcher (${PACKAGE_NAME})..."
gcc -O2 "${SCRIPT_DIR}/src/launcher.c" -o "${STAGING_DIR}/usr/bin/${PACKAGE_NAME}"
chmod 0755 "${STAGING_DIR}/usr/bin/${PACKAGE_NAME}"

# Install Desktop file as deadbeef-coverflow.desktop
cp "${SCRIPT_DIR}/deadbeef-coverflow.desktop" "${STAGING_DIR}/usr/share/applications/"

# Install Pixmap and Icons named deadbeef-coverflow
if [ -f "${BASE_SRC}/deadbeef.png" ]; then
    cp "${BASE_SRC}/deadbeef.png" "${STAGING_DIR}/usr/share/pixmaps/${PACKAGE_NAME}.png"
fi

for sz in 16x16 22x22 24x24 32x32 48x48 64x64 128x128 192x192 256x256; do
    if [ -d "${SCRIPT_DIR}/icons/${sz}" ]; then
        mkdir -p "${STAGING_DIR}/usr/share/icons/hicolor/${sz}/apps"
        if [ -f "${SCRIPT_DIR}/icons/${sz}/deadbeef.png" ]; then
            cp "${SCRIPT_DIR}/icons/${sz}/deadbeef.png" "${STAGING_DIR}/usr/share/icons/hicolor/${sz}/apps/${PACKAGE_NAME}.png"
        fi
    fi
done

if [ -d "${SCRIPT_DIR}/icons/scalable" ]; then
    mkdir -p "${STAGING_DIR}/usr/share/icons/hicolor/scalable/apps"
    if [ -f "${SCRIPT_DIR}/icons/scalable/deadbeef.svg" ]; then
        cp "${SCRIPT_DIR}/icons/scalable/deadbeef.svg" "${STAGING_DIR}/usr/share/icons/hicolor/scalable/apps/${PACKAGE_NAME}.svg"
    fi
fi

# Set standard permissions
find "${STAGING_DIR}" -type d -exec chmod 0755 {} +
chmod 0755 "${STAGING_DIR}/opt/${PACKAGE_NAME}/deadbeef"
chmod 0755 "${STAGING_DIR}/opt/${PACKAGE_NAME}/plugins/"*.so
chmod 0755 "${STAGING_DIR}/opt/${PACKAGE_NAME}/lib/"*.so*

# --- 6. Debian Control & Scripts ---
echo "[5/6] Creating Debian package control files..."
INSTALLED_SIZE="$(du -sk "${STAGING_DIR}" | cut -f1)"

cat << EOF > "${STAGING_DIR}/DEBIAN/control"
Package: ${PACKAGE_NAME}
Version: ${VERSION}
Architecture: ${ARCH}
Maintainer: sophAi <clusterga@gmail.com>
Installed-Size: ${INSTALLED_SIZE}
Depends: libc6 (>= 2.34), libgtk-3-0t64 (>= 3.24.0) | libgtk-3-0, libepoxy0 (>= 1.4.3), libgl1, libasound2t64 | libasound2, libpulse0, zlib1g
Section: sound
Priority: optional
Homepage: https://github.com/sophAi/deadbeef-coverflow
Description: DeaDBeeF music player with 3D OpenGL Cover Flow plugin
 DeaDBeeF is a modular, fast, and lightweight audio player.
 This package provides DeaDBeeF with the native 3D OpenGL Cover Flow plugin,
 installed as '${PACKAGE_NAME}' to allow coexisting alongside any
 system DeaDBeeF package without conflicts.
EOF

cat << 'EOF' > "${STAGING_DIR}/DEBIAN/postinst"
#!/bin/sh
set -e

if [ "$1" = "configure" ]; then
    if which update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q /usr/share/applications || true
    fi
    if which gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
    fi
fi

exit 0
EOF
chmod 0755 "${STAGING_DIR}/DEBIAN/postinst"

cat << 'EOF' > "${STAGING_DIR}/DEBIAN/postrm"
#!/bin/sh
set -e

if [ "$1" = "remove" ] || [ "$1" = "purge" ]; then
    if which update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q /usr/share/applications || true
    fi
    if which gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
    fi
fi

exit 0
EOF
chmod 0755 "${STAGING_DIR}/DEBIAN/postrm"

# Generate MD5 checksums
cd "${STAGING_DIR}"
find opt usr -type f -exec md5sum {} + > DEBIAN/md5sums
chmod 0644 DEBIAN/md5sums DEBIAN/control

# --- 7. Build .deb Package ---
echo "[6/6] Generating Debian (.deb) package with dpkg-deb..."
dpkg-deb --root-owner-group --build "${STAGING_DIR}" "${OUTPUT_DEB}"

# Cleanup
rm -rf "${STAGING_DIR}"

# Copy newly created .deb to parent directory if in workspace
PARENT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
if [ -d "${PARENT_DIR}" ] && [ "${PARENT_DIR}" != "${SCRIPT_DIR}" ]; then
    cp -u "${OUTPUT_DEB}" "${PARENT_DIR}/" || true
fi

echo ""
echo "===================================================================="
echo " Successfully created: ${OUTPUT_DEB}"
echo " Command to run: ${PACKAGE_NAME}"
echo "===================================================================="
dpkg-deb -I "${OUTPUT_DEB}"
