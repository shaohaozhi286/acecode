"""从单张源 PNG 重新生成 ACECode 全套图标资产。

输入: 命令行第一个参数指向源 PNG(任意分辨率,推荐 >= 256×256,正方形最佳)。
输出:
  - assets/windows/acecode_icon.png  : 1024×1024 母版
  - assets/windows/acecode.ico       : 多尺寸 ICO(16/24/32/48/64/128/256)
  - web/public/acecode-logo.png      : 256×256,前端 TopBar / 欢迎屏使用
  - web/public/favicon.ico           : 16/32/48 favicon

用法:
  python scripts/regenerate_icons.py "C:\\path\\to\\source.png"
"""

import sys
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
WIN_PNG = ROOT / "assets" / "windows" / "acecode_icon.png"
WIN_ICO = ROOT / "assets" / "windows" / "acecode.ico"
WEB_LOGO = ROOT / "web" / "public" / "acecode-logo.png"
WEB_FAVICON = ROOT / "web" / "public" / "favicon.ico"

ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]
FAVICON_SIZES = [16, 32, 48]
MASTER_SIZE = 1024
WEB_LOGO_SIZE = 256


def load_square_rgba(src_path: Path) -> Image.Image:
    img = Image.open(src_path).convert("RGBA")
    if img.width != img.height:
        side = max(img.width, img.height)
        canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        canvas.paste(img, ((side - img.width) // 2, (side - img.height) // 2))
        img = canvas
    return img


def resize(img: Image.Image, size: int) -> Image.Image:
    return img.resize((size, size), Image.LANCZOS)


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: regenerate_icons.py <source.png>", file=sys.stderr)
        return 2

    src = Path(sys.argv[1])
    if not src.exists():
        print(f"source not found: {src}", file=sys.stderr)
        return 1

    base = load_square_rgba(src)
    master = resize(base, MASTER_SIZE) if base.width != MASTER_SIZE else base

    WIN_PNG.parent.mkdir(parents=True, exist_ok=True)
    master.save(WIN_PNG, format="PNG", optimize=True)
    print(f"wrote {WIN_PNG} ({master.size[0]}x{master.size[1]})")

    # Pillow 的 ICO encoder 会从 base 图自己下采到 sizes 列表里的每一档,
    # 所以必须用高分辨率(>= 最大 size)作 base,否则小图被上采,所有尺寸都糊。
    master.save(WIN_ICO, format="ICO", sizes=[(s, s) for s in ICO_SIZES])
    print(f"wrote {WIN_ICO} (sizes: {ICO_SIZES})")

    WEB_LOGO.parent.mkdir(parents=True, exist_ok=True)
    web_logo = resize(master, WEB_LOGO_SIZE)
    web_logo.save(WEB_LOGO, format="PNG", optimize=True)
    print(f"wrote {WEB_LOGO} ({WEB_LOGO_SIZE}x{WEB_LOGO_SIZE})")

    master.save(WEB_FAVICON, format="ICO", sizes=[(s, s) for s in FAVICON_SIZES])
    print(f"wrote {WEB_FAVICON} (sizes: {FAVICON_SIZES})")

    return 0


if __name__ == "__main__":
    sys.exit(main())
