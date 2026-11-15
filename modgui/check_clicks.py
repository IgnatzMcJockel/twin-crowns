#!/usr/bin/env python3
"""Checks in Chrome that a click on every control reaches it, instead of MOD-UI's drag handle.

Run from the repo root in WSL. Adds the one rule of MOD-UI's pedals.css that decides this, which
screenshots can't show, because the drag handle is invisible.
"""
import re
import subprocess
import sys

import make_screenshots as render

MOD_UI_RULE = ".mod-pedal .mod-drag-handle { z-index: 20; }"

CHECK = """
const lines = [];
document.querySelectorAll('[mod-role=input-control-port], [mod-role=bypass]').forEach(control => {
    const options = [...control.querySelectorAll('[mod-role=enumeration-option]')];
    (options.length ? options : [control]).forEach(target => {
        const box = target.getBoundingClientRect();
        const hit = document.elementFromPoint(box.left + box.width / 2, box.top + box.height / 2);
        const ok = hit !== null && (hit === target || target.contains(hit));
        lines.push((ok ? 'ok      ' : 'BLOCKED ') + (control.getAttribute('mod-port-symbol') || 'bypass') + ' '
                   + target.textContent.trim() + (ok ? '' : '  (hits ' + (hit ? hit.className : 'nothing') + ')'));
    });
});
document.body.innerHTML = '<pre id="' + 'result">' + lines.join('\\n') + '</' + 'pre>';
"""


def main():
    html = f"{render.BUILD}/check_clicks.html"

    with open(html, "w") as f:
        f.write(render.page(1).replace("</style>", MOD_UI_RULE + "</style>").replace("</script>", CHECK + "</script>"))

    dom = subprocess.run([render.CHROME, "--headless=new", "--disable-gpu", "--force-device-scale-factor=1",
                          f"--window-size={render.WIDTH + 200},{render.HEIGHT + 200}", "--dump-dom",
                          "file:///" + render.windows_path(html).replace("\\", "/")],
                         check=True, capture_output=True, text=True, timeout=60).stdout

    lines = re.search(r'<pre id="result">(.*?)</pre>', dom, re.S).group(1).split("\n")
    blocked = [line for line in lines if not line.startswith("ok")]
    print("\n".join(blocked))
    print(f"{len(lines) - len(blocked)} of {len(lines)} controls reachable")
    sys.exit(1 if blocked else 0)


if __name__ == "__main__":
    main()
