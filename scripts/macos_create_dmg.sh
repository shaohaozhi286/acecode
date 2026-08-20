#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage:
  scripts/macos_create_dmg.sh --app <ACECode.app> --output <ACECode.dmg> \
      [--volume-name <name>] [--background <path>]

Creates a styled compressed read-only DMG with ACECode.app on the left and a
standard Applications link on the right.
USAGE
}

app_path=""
output_path=""
volume_name="ACECode"
background_path=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --app)
            [[ $# -ge 2 && -n "${2:-}" ]] || { echo "Missing value for --app" >&2; exit 2; }
            app_path="$2"
            shift 2
            ;;
        --output)
            [[ $# -ge 2 && -n "${2:-}" ]] || { echo "Missing value for --output" >&2; exit 2; }
            output_path="$2"
            shift 2
            ;;
        --volume-name)
            [[ $# -ge 2 && -n "${2:-}" ]] || { echo "Missing value for --volume-name" >&2; exit 2; }
            volume_name="$2"
            shift 2
            ;;
        --background)
            [[ $# -ge 2 && -n "${2:-}" ]] || { echo "Missing value for --background" >&2; exit 2; }
            background_path="$2"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "DMG creation must run on macOS." >&2
    exit 1
fi

if [[ -z "$app_path" || -z "$output_path" ]]; then
    echo "--app and --output are required." >&2
    usage >&2
    exit 2
fi

if [[ "$output_path" != *.dmg ]]; then
    echo "DMG output must end in .dmg: $output_path" >&2
    exit 2
fi

if [[ ! -d "$app_path" ]]; then
    echo "Missing ACECode app bundle: $app_path" >&2
    exit 1
fi
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -z "$background_path" ]]; then
    background_path="$script_dir/../assets/macos/acecode-dmg-background.svg"
fi
volume_icon_path="$script_dir/../assets/macos/acecode.icns"

if [[ ! -f "$background_path" ]]; then
    echo "Missing DMG background: $background_path" >&2
    exit 1
fi
if [[ ! -f "$volume_icon_path" ]]; then
    echo "Missing DMG volume icon: $volume_icon_path" >&2
    exit 1
fi

output_parent="$(dirname "$output_path")"
mkdir -p "$output_parent"

temporary_base="${TMPDIR:-/tmp}"
temporary_base="${temporary_base%/}"
temporary_root="$(mktemp -d "${temporary_base}/acecode-dmg.XXXXXX")"
mount_root=""
detach_target=""
mounted=0
cleanup() {
    if [[ "${mounted:-0}" -eq 1 ]]; then
        local cleanup_target="${mount_root:-${detach_target:-}}"
        if [[ -n "$cleanup_target" ]]; then
            /usr/bin/hdiutil detach "$cleanup_target" -force >/dev/null 2>&1 || true
        fi
    fi
    if [[ -n "${temporary_root:-}" && -d "$temporary_root" ]]; then
        rm -rf -- "$temporary_root"
    fi
}
trap cleanup EXIT

detach_with_retry() {
    local target="$1"
    local detach_log="$temporary_root/detach.log"
    local attempt

    for attempt in 1 2 3 4 5 6; do
        if /usr/bin/hdiutil detach "$target" >"$detach_log" 2>&1; then
            return 0
        fi
        echo "DMG detach attempt $attempt/6 failed:" >&2
        sed -n '1,80p' "$detach_log" >&2
        sleep "$attempt"
    done

    echo "DMG remained busy; forcing the final detach attempt." >&2
    /usr/bin/hdiutil detach "$target" -force
}

staging_root="$temporary_root/root"
background_root="$staging_root/.background"
mkdir -p "$staging_root" "$background_root"

/usr/bin/ditto "$app_path" "$staging_root/ACECode.app"
/bin/ln -s /Applications "$staging_root/Applications"
background_png="$background_root/ACECode-DMG.png"
# Older macOS sips (e.g. 12.x) silently fails to rasterize SVG and writes no
# output while still returning success. Fall back to qlmanage when the expected
# PNG is missing or not a valid image so the Finder background still resolves.
/usr/bin/sips -s format png "$background_path" \
    --out "$background_png" >/dev/null 2>&1 || true
if [[ ! -s "$background_png" ]]; then
    echo "sips could not rasterize the SVG background; using qlmanage fallback" >&2
    if ! /usr/bin/qlmanage -t -s 660 -o "$background_root" "$background_path" >/dev/null 2>&1; then
        echo "Could not rasterize the DMG background image." >&2
        exit 1
    fi
    ql_png="$background_root/$(basename "$background_path").png"
    if [[ ! -f "$ql_png" ]]; then
        echo "qlmanage produced no background image." >&2
        exit 1
    fi
    /bin/mv -f "$ql_png" "$background_png"
fi
if [[ ! -s "$background_png" ]]; then
    echo "Missing DMG background raster: $background_png" >&2
    exit 1
fi

if [[ ! -L "$staging_root/Applications" ]] || \
   [[ "$(/usr/bin/readlink "$staging_root/Applications")" != "/Applications" ]]; then
    echo "DMG Applications link must point exactly to /Applications." >&2
    exit 1
fi

staging_size_kb="$(/usr/bin/du -sk "$staging_root" | /usr/bin/awk '{print $1}')"
image_size_mb=$(( (staging_size_kb + 1023) / 1024 + 48 ))
writable_dmg="$temporary_root/ACECode-layout.dmg"

/usr/bin/hdiutil create \
    -volname "$volume_name" \
    -srcfolder "$staging_root" \
    -size "${image_size_mb}m" \
    -fs HFS+ \
    -format UDRW \
    -ov \
    "$writable_dmg"

attach_plist="$temporary_root/attach.plist"
/usr/bin/hdiutil attach \
    -readwrite \
    -noverify \
    -noautoopen \
    -plist \
    "$writable_dmg" >"$attach_plist"
mounted=1

for entity_index in 0 1 2 3 4 5 6 7; do
    device_candidate="$(/usr/libexec/PlistBuddy \
        -c "Print :system-entities:${entity_index}:dev-entry" \
        "$attach_plist" 2>/dev/null || true)"
    if [[ -n "$device_candidate" && -z "$detach_target" ]]; then
        detach_target="$device_candidate"
    fi
    mount_candidate="$(/usr/libexec/PlistBuddy \
        -c "Print :system-entities:${entity_index}:mount-point" \
        "$attach_plist" 2>/dev/null || true)"
    if [[ -n "$mount_candidate" ]]; then
        mount_root="$mount_candidate"
        if [[ -n "$device_candidate" ]]; then
            detach_target="$device_candidate"
        fi
        break
    fi
done
if [[ -z "$mount_root" || ! -d "$mount_root" ]]; then
    echo "Could not determine the mounted DMG path." >&2
    exit 1
fi

# Keep the background resources out of the visible Finder layout.
/usr/bin/SetFile -a V "$mount_root/.background"

apply_finder_layout() {
    /usr/bin/osascript - "$mount_root" <<'APPLESCRIPT'
on run argv
    set expectedMountPath to item 1 of argv

    tell application "Finder"
        activate
        set mountedFolder to folder (POSIX file expectedMountPath as alias)
        tell mountedFolder
            open
            set dmgWindow to container window
            set current view of dmgWindow to icon view
            set toolbar visible of dmgWindow to false
            set statusbar visible of dmgWindow to false
            set pathbar visible of dmgWindow to false
            set bounds of dmgWindow to {100, 100, 760, 500}

            set iconOptions to icon view options of dmgWindow
            set arrangement of iconOptions to not arranged
            set icon size of iconOptions to 112
            set text size of iconOptions to 13
            set label position of iconOptions to bottom
            set shows icon preview of iconOptions to false
            set background picture of iconOptions to file ".background:ACECode-DMG.png"

            set position of item "ACECode.app" of dmgWindow to {160, 215}
            set position of item "Applications" of dmgWindow to {500, 215}

            update without registering applications
            delay 2
            close dmgWindow
        end tell
    end tell
end run
APPLESCRIPT
}

layout_log="$temporary_root/finder-layout.log"
layout_applied=0
for attempt in 1 2 3; do
    if apply_finder_layout >"$layout_log" 2>&1; then
        layout_applied=1
        break
    fi
    echo "Finder layout attempt $attempt failed:" >&2
    sed -n '1,120p' "$layout_log" >&2
    sleep 2
done
if [[ "$layout_applied" -ne 1 ]]; then
    echo "Could not apply the ACECode DMG Finder layout." >&2
    exit 1
fi

# Finder may clean volume-icon metadata while first creating .DS_Store, so add
# the custom mounted-volume icon only after the layout window has closed.
/usr/bin/ditto "$volume_icon_path" "$mount_root/.VolumeIcon.icns"
/usr/bin/SetFile -a V "$mount_root/.VolumeIcon.icns"
/usr/bin/SetFile -a C "$mount_root"
if [[ ! -f "$mount_root/.VolumeIcon.icns" ]]; then
    echo "Could not add the DMG volume icon." >&2
    exit 1
fi

/bin/sync
detach_target="${detach_target:-$mount_root}"
detach_with_retry "$detach_target"
mounted=0

/usr/bin/hdiutil convert "$writable_dmg" \
    -format UDZO \
    -imagekey zlib-level=9 \
    -ov \
    -o "$output_path"

/usr/bin/hdiutil verify "$output_path"
echo "Created $output_path"
