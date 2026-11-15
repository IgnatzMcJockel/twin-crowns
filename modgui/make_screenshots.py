#!/usr/bin/env python3
"""Renders the modgui with Chrome into modgui/screenshot-crowns.png and modgui/thumbnail-crowns.png.

Run from the repo root in WSL after changing the layout. MOD-UI uses the two images in its plugin
browser, so they are kept in the repo. The page fakes what MOD-UI's widgets do at the default values.
"""
import os
import re
import subprocess
import sys

CHROME = "/mnt/c/Program Files/Google/Chrome/Application/chrome.exe"
BUILD = "build/modgui-preview"
ART = "third_party/MoT/assets/ui"
WIDTH, HEIGHT = 640, 446
THUMBNAIL_WIDTH = 200

DEFAULTS = """
document.querySelectorAll('[mod-widget=switch]').forEach(e => e.classList.add('on'));
document.querySelectorAll('.crowns-clip [mod-port-value="1"]').forEach(e => e.classList.add('selected'));
document.querySelector('.crowns-supply [mod-port-value="0"]').classList.add('selected');
document.querySelectorAll('.crowns-presence').forEach(e => e.style.transform = 'rotate(-135deg)');
"""


def windows_path(path):
    return subprocess.check_output(["wslpath", "-w", os.path.abspath(path)], text=True).strip()


# MOD-UI's main.css forces its font on every element; the page needs it to show what MOD-UI shows.
MOD_UI_CSS = '* { font-family: "cooper hewitt", sans-serif !important; }'


def resource(name):
    """The bundle's modgui folder holds both, so a resource is ours in modgui/ or MoT's art."""
    path = f"modgui/{name}" if os.path.exists(f"modgui/{name}") else f"{ART}/{name}"
    return os.path.relpath(path, BUILD).replace(os.sep, "/")


def page(scale):
    with open("modgui/icon-crowns.html") as f:
        icon = f.read()

    with open("modgui/stylesheet-crowns.css") as f:
        css = f.read()

    icon = re.sub(r"\{\{#effect[^}]*\}\}.*?\{\{/effect[^}]*\}\}", "", icon, flags=re.S)
    icon = icon.replace("{{{cns}}}", "")
    css = css.replace("{{{cns}}}", "").replace("{{{ns}}}", "")
    css = re.sub(r"/resources/([^)]+)", lambda m: resource(m.group(1)), css)
    return (f"<!DOCTYPE html><html><head><style>html, body {{ margin: 0; background: transparent; overflow: hidden; }}"
            f"#zoom {{ zoom: {scale}; }} {MOD_UI_CSS} {css}</style></head><body><div id='zoom'>{icon}</div>"
            f"<script>{DEFAULTS}</script></body></html>")


def screenshot(name, scale, width, height):
    html = f"{BUILD}/{name}.html"

    with open(html, "w") as f:
        f.write(page(scale))

    subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                    "--default-background-color=00000000", "--allow-file-access-from-files",
                    "--virtual-time-budget=3000",  # lets the fonts load before the shot
                    f"--window-size={width},{height}", f"--screenshot={windows_path(f'modgui/{name}-crowns.png')}",
                    "file:///" + windows_path(html).replace("\\", "/")],
                   check=True, capture_output=True, timeout=60)
    print(f"modgui/{name}-crowns.png  {os.path.getsize(f'modgui/{name}-crowns.png') // 1024} KB")


def main():
    if not os.path.exists(CHROME):
        sys.exit("Chrome not found")

    os.makedirs(BUILD, exist_ok=True)
    screenshot("screenshot", 1, WIDTH, HEIGHT)
    screenshot("thumbnail", THUMBNAIL_WIDTH / WIDTH, THUMBNAIL_WIDTH, HEIGHT * THUMBNAIL_WIDTH // WIDTH)


if __name__ == "__main__":
    main()
