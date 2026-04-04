# zebra-cups-mono

Crisp 1-bit monochrome printing for Zebra label printers on macOS.

## The problem

macOS renders PDFs through Quartz, which applies anti-aliasing (grayscale smoothing). Zebra thermal printers are 1-bit — they can only print pure black or white dots. When the default CUPS filter receives this anti-aliased grayscale raster, it dithers it, producing blurry text and barcodes.

Previously I used solutions like an [Automator Script graciously provided by others](https://github.com/john-stephens/zebra-mac-label-automator) but that required extra clicks, didn't work in web browsers that used their own print dialogs, and was slow if you had a bunch of labels to print. I _only_ use this machine for shipping label printing so I wanted to make it as dead-simple and fast as possible. 

## The fix

A native CUPS filter that bypasses the macOS rendering pipeline entirely. It uses CoreGraphics directly with **all anti-aliasing disabled**, renders to an 8-bit grayscale bitmap, applies a hard black/white threshold, and outputs ZPL `^GFA` commands straight to the printer.

No Ghostscript, no ImageMagick, no Python — just a compiled C binary using macOS system frameworks. This is necessary because macOS sandboxes CUPS filters and blocks execution of Homebrew binaries.

## Install

Requires Xcode command line tools (`xcode-select --install`).

```bash
# Compile
cc -framework CoreGraphics -framework CoreFoundation -O2 \
    -o zebra-mono-filter zebra-mono-filter.c

# Install filter
sudo cp zebra-mono-filter /usr/libexec/cups/filter/zebra-mono-filter
sudo chmod 755 /usr/libexec/cups/filter/zebra-mono-filter

# Install PPD (backs up original first)
sudo cp /etc/cups/ppd/ZD621.ppd /etc/cups/ppd/ZD621.ppd.bak
sudo cp ZD621-4x6.ppd /etc/cups/ppd/ZD621.ppd

# Restart CUPS
sudo launchctl stop org.cups.cupsd && sudo launchctl start org.cups.cupsd
```

## How it works

1. Any app prints to the Zebra printer via the normal macOS print dialog
2. CUPS routes the PDF through `zebra-mono-filter`
3. The filter renders the PDF page using CoreGraphics with anti-aliasing, font smoothing, and interpolation all disabled
4. Each pixel is thresholded: gray value < 128 = black, otherwise white
5. The 1-bit bitmap is encoded as a ZPL `^GFA` (Graphic Field) command
6. ZPL goes directly to the printer over the network socket

## Configuration

The filter is hardcoded for 4x6" labels at 203 DPI. To change, edit the defines at the top of `zebra-mono-filter.c` and recompile:

```c
#define DPI 203
#define LABEL_W_IN 4.0
#define LABEL_H_IN 6.0
#define THRESHOLD 128   // 0-255, lower = more black
```

## Compatibility

- macOS (tested on Sequoia)
- Zebra ZPL printers (ZD621, ZD420, ZT410, etc.)
- 203 DPI, 4x6" labels by default
