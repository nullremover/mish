
#!/bin/bash
set -e

REPO="https://raw.githubusercontent.com/nullremover/mish/main"
ROOT="${GITHUB_WORKSPACE:-$(pwd)}"

echo "Mish macOS test setup"

if [ ! -f "$ROOT/Makefile.macos" ]; then
    echo "ERROR: Run this inside the Mish source directory."
    exit 1
fi

echo "Creating test music folder..."
mkdir -p "$ROOT/test-music"

echo "Downloading sample MP3..."
curl -fL --retry 3 \
  "https://www.soundhelix.com/examples/mp3/SoundHelix-Song-1.mp3" \
  -o "$ROOT/test-music/test.mp3"

echo "Writing music folder path to config..."
printf '%s\n' "$ROOT/test-music" > "$ROOT/mish_config.txt"

echo "Checking downloaded file..."
file "$ROOT/test-music/test.mp3"
ls -lh "$ROOT/test-music/test.mp3"

echo "Rebuilding Mish for Intel..."
make -C "$ROOT" -f Makefile.macos clean
make -C "$ROOT" -f Makefile.macos ARCH=x86_64

echo "Build complete!"
echo "Executable: $ROOT/builds/mishplayer-x86_64"
echo "Config: $ROOT/mish_config.txt"

echo "Attempting launch..."
"$ROOT/builds/mishplayer-x86_64" || {
    echo "App exited with an error. A headless VM may not have a graphical display."
}
