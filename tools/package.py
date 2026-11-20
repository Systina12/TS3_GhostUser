"""Package an existing Windows DLL without publishing it."""
from pathlib import Path
import re
import shutil
from zipfile import ZIP_DEFLATED, ZipFile

root = Path(__file__).resolve().parent.parent
dist = root / "dist"
dll = dist / "ghostuser.dll"
assert dll.is_file(), "Build dist/ghostuser.dll first"
match = re.search(r"project\(TS3GhostUser VERSION ([0-9.]+)", (root / "CMakeLists.txt").read_text())
assert match, "Missing package version"
package = dist / f"GhostUser-v{match[1]}-windows-x64.zip"
with ZipFile(package, "w", ZIP_DEFLATED) as archive:
    archive.write(dll, "ghostuser.dll")
    archive.write(root / "README.md", "README.md")
shutil.copyfile(package, dist / "GhostUser-windows-x64.zip")
print(package)
