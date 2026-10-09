#!/usr/bin/env bash
# ==============================================================================
# DeaDBeeF Cover Flow Plugin - Standalone Build & Install Script
#
# Compiles the 3D OpenGL Cover Flow GTK3 plugin (coverflow_gtk3.so) independently
# without requiring a full recompilation of the DeaDBeeF core.
# ==============================================================================

set -euo pipefail

# ANSI color codes
BOLD="\033[1m"
GREEN="\033[0;32m"
BLUE="\033[0;34m"
YELLOW="\033[1;33m"
RED="\033[0;31m"
RESET="\033[0m"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_DIR="${SCRIPT_DIR}/plugins/coverflow"
TARGET_SO="${PLUGIN_DIR}/coverflow_gtk3.so"
USER_PLUGIN_DIR="${HOME}/.local/lib/deadbeef"
SYSTEM_PLUGIN_DIR="/usr/lib/deadbeef"

DO_INSTALL=true
INSTALL_DEST="${USER_PLUGIN_DIR}"

show_help() {
    echo -e "${BOLD}DeaDBeeF Cover Flow Plugin Builder${RESET}

Usage: $(basename "$0") [OPTIONS]

Options:
  -i, --install          Install plugin to user directory (~/.local/lib/deadbeef) [Default]
  -s, --system           Install plugin system-wide (/usr/lib/deadbeef, requires root/sudo)
  -d, --dest <DIR>       Install plugin to a custom destination directory
  -n, --no-install       Build only, do not install
  -c, --clean            Clean build artifacts only
  -h, --help             Show this help message

Examples:
  ./$(basename "$0")                     # Build and install to ~/.local/lib/deadbeef
  ./$(basename "$0") --system            # Build and install system-wide
  ./$(basename "$0") --no-install        # Only compile coverflow_gtk3.so"
}

# Parse command line options
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            show_help
            exit 0
            ;;
        -c|--clean)
            echo -e "${BLUE}Cleaning build artifacts...${RESET}"
            make -C "${PLUGIN_DIR}" clean
            echo -e "${GREEN}Clean completed.${RESET}"
            exit 0
            ;;
        -i|--install)
            DO_INSTALL=true
            INSTALL_DEST="${USER_PLUGIN_DIR}"
            shift
            ;;
        -s|--system)
            DO_INSTALL=true
            INSTALL_DEST="${SYSTEM_PLUGIN_DIR}"
            shift
            ;;
        -d|--dest)
            if [ -n "${2:-}" ]; then
                DO_INSTALL=true
                INSTALL_DEST="$2"
                shift 2
            else
                echo -e "${RED}Error: --dest requires a directory argument.${RESET}" >&2
                exit 1
            fi
            ;;
        -n|--no-install)
            DO_INSTALL=false
            shift
            ;;
        *)
            echo -e "${RED}Unknown option: $1${RESET}" >&2
            show_help
            exit 1
            ;;
    esac
done

echo -e "${BOLD}${BLUE}====================================================================${RESET}"
echo -e "${BOLD} Building DeaDBeeF Cover Flow GTK3 Plugin (OpenGL 3D)${RESET}"
echo -e "${BOLD}${BLUE}====================================================================${RESET}"

# 1. Check build tools and dependencies
echo -e "${BOLD}[1/3] Checking prerequisites...${RESET}"

MISSING_DEPS=()
for cmd in gcc make pkg-config; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        MISSING_DEPS+=("$cmd")
    fi
done

if ! pkg-config --exists gtk+-3.0; then
    MISSING_DEPS+=("libgtk-3-dev")
fi

if ! pkg-config --exists epoxy; then
    MISSING_DEPS+=("libepoxy-dev")
fi

if [ ${#MISSING_DEPS[@]} -gt 0 ]; then
    echo -e "${RED}Missing required dependencies:${RESET} ${MISSING_DEPS[*]}" >&2
    echo -e "\nPlease install them using apt:"
    echo -e "  ${YELLOW}sudo apt update && sudo apt install -y build-essential pkg-config libgtk-3-dev libepoxy-dev${RESET}\n"
    exit 1
fi
echo -e "${GREEN}All required build tools and libraries found.${RESET}"

# 2. Compile plugin
echo -e "\n${BOLD}[2/3] Compiling plugin...${RESET}"
make -C "${PLUGIN_DIR}" clean
make -C "${PLUGIN_DIR}"

if [ ! -f "${TARGET_SO}" ]; then
    echo -e "${RED}Error: Compilation succeeded but ${TARGET_SO} was not found.${RESET}" >&2
    exit 1
fi
echo -e "${GREEN}Successfully compiled:${RESET} ${TARGET_SO}"

# Compile notify plugin if available
NOTIFY_DIR="${SCRIPT_DIR}/plugins/notify"
NOTIFY_SO="${NOTIFY_DIR}/notify.so"
if [ -d "${NOTIFY_DIR}" ]; then
    echo -e "${BLUE}Compiling OSD Notify plugin...${RESET}"
    make -C "${NOTIFY_DIR}" clean
    make -C "${NOTIFY_DIR}"
fi

# Compile video plugin if available
VIDEO_DIR="${SCRIPT_DIR}/plugins/video"
VIDEO_SO="${VIDEO_DIR}/video_gtk3.so"
if [ -d "${VIDEO_DIR}" ]; then
    echo -e "${BLUE}Compiling Video & Cover Player plugin...${RESET}"
    make -C "${VIDEO_DIR}" clean
    make -C "${VIDEO_DIR}"
fi

# 3. Install if requested
if [ "$DO_INSTALL" = true ]; then
    echo -e "\n${BOLD}[3/3] Installing plugins to:${RESET} ${INSTALL_DEST}"
    if [ ! -d "${INSTALL_DEST}" ]; then
        if [ "$INSTALL_DEST" = "${USER_PLUGIN_DIR}" ]; then
            mkdir -p "${INSTALL_DEST}"
        else
            echo -e "${YELLOW}Directory ${INSTALL_DEST} does not exist. Creating with sudo...${RESET}"
            sudo mkdir -p "${INSTALL_DEST}"
        fi
    fi

    if [ -w "${INSTALL_DEST}" ]; then
        cp -v "${TARGET_SO}" "${INSTALL_DEST}/"
        if [ -f "${NOTIFY_SO}" ]; then
            cp -v "${NOTIFY_SO}" "${INSTALL_DEST}/"
        fi
        if [ -f "${VIDEO_SO}" ]; then
            cp -v "${VIDEO_SO}" "${INSTALL_DEST}/"
        fi
    else
        echo -e "${YELLOW}Destination requires elevated permissions, using sudo...${RESET}"
        sudo cp -v "${TARGET_SO}" "${INSTALL_DEST}/"
        if [ -f "${NOTIFY_SO}" ]; then
            sudo cp -v "${NOTIFY_SO}" "${INSTALL_DEST}/"
        fi
        if [ -f "${VIDEO_SO}" ]; then
            sudo cp -v "${VIDEO_SO}" "${INSTALL_DEST}/"
        fi
    fi

    echo -e "\n${BOLD}${GREEN}✔ Installation complete!${RESET}"
    echo -e "Plugins installed to: ${BOLD}${INSTALL_DEST}/${RESET}"
else
    echo -e "\n${BOLD}[3/3] Skipping installation (--no-install specified).${RESET}"
    echo -e "Plugin binary available at: ${BOLD}${TARGET_SO}${RESET}"
fi

echo -e "\n${BOLD}How to enable Cover Flow in DeaDBeeF:${RESET}"
echo -e "  1. Launch DeaDBeeF: ${YELLOW}deadbeef${RESET} (or ${YELLOW}deadbeef-coverflow${RESET})"
echo -e "  2. In the top menu, go to: ${BOLD}View -> Design Mode${RESET} (檢視 -> 設計模式)"
echo -e "  3. Right-click on a layout panel and select: ${BOLD}Insert New Widget -> Cover Flow${RESET}"
echo -e "  4. Uncheck ${BOLD}View -> Design Mode${RESET} to save and enjoy 3D Cover Flow!"
echo -e "${BOLD}${BLUE}====================================================================${RESET}"
