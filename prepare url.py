#!/data/data/com.termux/files/usr/bin/python
import os
import subprocess
from collections import defaultdict

playlist_url = input("🎵 Enter YouTube Music playlist or channel URL: ").strip()
if not playlist_url:
    print("❌ No URL provided. Exiting.")
    exit(1)
fold=input("enter destination_)
dest = os.path.expanduser("~/storage/shared/music/"+fold)
os.makedirs(dest, exist_ok=True)
outfile = os.path.join(dest, "url.txt")

print("🔍 Fetching titles & URLs...")

# Extract IDs and titles
cmd = [
    "yt-dlp",
    "--flat-playlist",
    "--print", "%(id)s|%(title)s",
    playlist_url
]

result = subprocess.run(cmd, capture_output=True, text=True, check=True)
lines = result.stdout.strip().splitlines()

# Track duplicates
title_counts = defaultdict(int)
output_lines = []

for line in lines:
    try:
        vid, title = line.split("|", 1)
    except ValueError:
        continue

    safe_title = (
        title.replace("/", "-")
             .replace("?", "")
             .replace(":", "")
    )

    title_counts[safe_title] += 1

    # Add counter if title repeats
    if title_counts[safe_title] > 1:
        safe_title = f"{safe_title}_{title_counts[safe_title]}"

    url = f"https://www.youtube.com/watch?v={vid}"
    output_lines.append(f"{safe_title}>{url}")

# Write url.txt
with open(outfile, "w") as f:
    for line in output_lines:
        f.write(line + "\n")

print(f"✅ Saved {len(output_lines)} unique titles to: {outfile}")
